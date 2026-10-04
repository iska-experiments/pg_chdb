/*
 * The worker's startup sweep.
 *
 * drop.c removes an index's store when the index is dropped and a database's
 * directory when the database is, but only from a backend that has the
 * library loaded when the drop's hook fires. The first DROP of a session that
 * loads it on demand, a session that never loads it, a crashed backend and a
 * COMMIT PREPARED all leave a store behind. So before a worker serves its
 * first request it lists the idx_<oid> databases of its store and drops those
 * whose OID is no longer a chdb index, and removes the pg_chdb/<dboid>
 * directory and socket of every database no longer in pg_database. Tables
 * inside a live index's store, old generations and staging tables, are
 * VACUUM's to sweep (vacuum.c).
 *
 * The worker queries its engine with the frames a backend would send
 * (frame.c) and decodes the Native answer as a scan does (scan.c).
 */

#include "postgres.h"

#include <stdlib.h>
#include <unistd.h>

#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

#include "../native.h"
#include "engine_proc.h"
#include "frame.h"
#include "protocol.h"
#include "search.h"
#include "sweep.h"

void
chdb_search_remove_store_dir(Oid dboid) {
    char path[MAXPGPATH];

    snprintf(path, sizeof(path), CHDB_SEARCH_DIR "/%u", dboid);
    if (!rmtree(path, true)) {
        ereport(
            WARNING,
            errmsg("chdb_search: could not remove store directory \"%s\"", path)
        );
    }
    snprintf(path, sizeof(path), CHDB_SEARCH_SOCKET_FMT, dboid);
    unlink(path);
}

/* ---- the engine, driven by the worker itself ---- */

/* Sends a request frame, raising with the engine's death if it cannot. */
static void
engine_request(chdbCmdType cmd, Oid index, const char* sql) {
    StringInfoData buf;
    char* dead = engine_ensure(MyDatabaseId);

    if (dead) {
        ereport(ERROR, errmsg("chdb_search: %s", dead));
    }
    initStringInfo(&buf);
    chdb_search_frame_request(&buf, cmd, index, 0, sql);
    if (!engine_send(buf.data, buf.len)) {
        ereport(ERROR, errmsg("chdb_search: %s", engine_death()));
    }
    pfree(buf.data);
}

/* Reads the status frame that ends every request, raising the engine's error. */
static void
engine_status(const char* sql) {
    chdbChannel* ch = engine_channel();
    uint8_t status;
    uint32_t len;

    chdb_channel_recv_exact(ch, &status, sizeof(status));
    chdb_channel_recv_exact(ch, &len, sizeof(len));
    if (len > CHDB_SEARCH_CHUNK_MAX) {
        ereport(ERROR, errmsg("chdb_search: the chDB engine sent a malformed reply"));
    }

    char* text = palloc(len + 1);

    chdb_channel_recv_exact(ch, text, len);
    text[len] = '\0';
    if (status != 0) {
        ereport(
            ERROR,
            errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
            errmsg("chdb_search: error executing chDB query"),
            errdetail("%s", text),
            errcontext("query: %s", sql)
        );
    }
}

/*
 * Runs a SELECT of one String column and returns its values. The engine's
 * data chunks are the chunked framing of channel.h, so the channel reads
 * them that way for the data and plainly again for the status after it.
 */
static List*
engine_names(const char* sql) {
    List* names = NIL;
    pgch_reader reader;
    void* state;
    Datum val;
    bool null;
    char skip[8192];

    engine_request(CHDB_CMD_SELECT, InvalidOid, sql);

    chdbChannel* ch = engine_channel();

    ch->chunked    = true;
    ch->chunk_left = 0;
    ch->data_ended = false;

    pgch_block_source src = chdb_native_source(ch);

    pgch_reader_init(&reader, &src);
    if (!reader.error && pgch_reader_columns(&reader) == 1) {
        state = pgch_reader_convert_init(&reader, 0, TEXTOID, -1);
        while (pgch_reader_next(&reader)) {
            pgch_reader_fill(&reader, &state, &val, &null);
            if (!null) {
                names = lappend(names, TextDatumGetCString(val));
            }
        }
    }
    if (reader.error) {
        ereport(
            ERROR,
            errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
            errmsg("chdb_search: %s", reader.error)
        );
    }
    /* To the end-of-data chunk, if the reader did not get there. */
    while (chdb_channel_recv(ch, skip, sizeof(skip))) {}
    ch->chunked = false;
    engine_status(sql);

    return names;
}

/* ---- the sweep ---- */

/*
 * An idx_<oid> store whose OID is not a chdb index is dropped. A build in
 * progress holds AccessExclusiveLock on its OID before the catalog shows the
 * index, so an OID that cannot be share-locked is kept, and the lock is held
 * across the drop so that no such build starts on it meanwhile. idx_0 is
 * the scratch database of the debug functions (debug.c).
 */
static void
sweep_orphan_stores(void) {
    List* dbs =
        engine_names("SELECT name FROM system.databases WHERE name LIKE 'idx\\\\_%'");
    ListCell* lc;

    foreach (lc, dbs) {
        const char* name  = lfirst(lc);
        char* end         = NULL;
        unsigned long oid = strtoul(name + strlen("idx_"), &end, 10);

        if (*end != '\0' || oid == 0 || oid != (Oid)oid) {
            continue;
        }
        if (!ConditionalLockRelationOid((Oid)oid, AccessShareLock)) {
            continue;
        }
        if (!chdb_search_is_index((Oid)oid)) {
            ereport(LOG, errmsg("chdb_search: swept orphan store idx_%u", (Oid)oid));
            engine_request(CHDB_CMD_DROP, (Oid)oid, "DROP");
            engine_status("DROP");
        }
        UnlockRelationOid((Oid)oid, AccessShareLock);
    }
}

/*
 * The store directory and socket of every database no longer in pg_database
 * go: a DROP DATABASE whose session had not loaded the library left them. Our
 * own database's and entries that are not <dboid> or <dboid>.sock stay.
 */
static void
sweep_orphan_dirs(Oid own) {
    DIR* dir = AllocateDir(CHDB_SEARCH_DIR);
    struct dirent* de;
    List* gone = NIL;
    ListCell* lc;

    if (!dir) {
        return; /* nothing created yet */
    }
    while ((de = ReadDir(dir, CHDB_SEARCH_DIR)) != NULL) {
        char* end;
        unsigned long dboid = strtoul(de->d_name, &end, 10);

        if (end == de->d_name || (*end != '\0' && strcmp(end, ".sock") != 0)) {
            continue;
        }
        if ((Oid)dboid != own) {
            gone = list_append_unique_oid(gone, (Oid)dboid);
        }
    }
    FreeDir(dir);

    foreach (lc, gone) {
        Oid dboid = lfirst_oid(lc);

        if (!SearchSysCacheExists1(DATABASEOID, ObjectIdGetDatum(dboid))) {
            ereport(
                LOG, errmsg("chdb_search: sweeping store of dropped database %u", dboid)
            );
            chdb_search_remove_store_dir(dboid);
        }
    }
}

/* The catalog lookups need a transaction; its memory takes the reads too. */
void
chdb_search_sweep(Oid dboid) {
    StartTransactionCommand();
    PG_TRY();
    {
        sweep_orphan_dirs(dboid);
        sweep_orphan_stores();
    }
    PG_CATCH();
    {
        /* The engine may be mid-reply, which the next request would read. */
        if (engine_pid() > 0) {
            pfree(engine_death());
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    CommitTransactionCommand();
}
