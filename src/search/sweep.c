/*
 * The worker's startup sweep.
 *
 * An index's store goes with the index and a database's store directory with
 * the database, but only when the backend dropping them has the library loaded
 * and gets to say so: one that loads it on demand inside the DROP, one that
 * never loads it, a crashed one and a COMMIT PREPARED all leave a store
 * behind. So before a worker serves its first request it lists the idx_<oid>
 * databases of its store and drops those whose OID is not a chdb index, and
 * removes the pg_chdb/<dboid> directory and socket of every database no
 * longer in pg_database. Tables inside a live index's store are the access
 * method's to sweep.
 *
 * The worker queries its engine with the frames a backend would send and
 * reads the status as a backend does (frame.c), and decodes the Native answer
 * as a backend does (native.c).
 */

#include "postgres.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

#include "../native.h"
#include "engine_proc.h"
#include "frame.h"
#include "protocol.h"
#include "sweep.h"

/* The access method's name, as sql/chdb_search.sql will create it. */
#define CHDB_SEARCH_AM "chdb"

bool
chdb_search_is_index(Oid relid) {
    HeapTuple tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));

    if (!HeapTupleIsValid(tup)) {
        return false;
    }

    Form_pg_class cls = (Form_pg_class)GETSTRUCT(tup);
    bool ours         = cls->relkind == RELKIND_INDEX && OidIsValid(cls->relam) &&
                        cls->relam == get_am_oid(CHDB_SEARCH_AM, true);

    ReleaseSysCache(tup);
    return ours;
}

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
    chdb_search_frame_request(&buf, cmd, index, sql);
    if (!engine_send(buf.data, buf.len)) {
        ereport(ERROR, errmsg("chdb_search: %s", engine_death()));
    }
    pfree(buf.data);
}

/* Reads the status frame that ends every request, raising the engine's error. */
static void
engine_status(const char* sql) {
    uint8_t status;
    char* text =
        chdb_search_frame_status(engine_channel(), "chDB engine", sql, &status);

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
    chdb_search_frame_skip_data(ch); /* if the reader did not get to the end */
    ch->chunked = false;
    /* The engine's own error explains a stream the reader could not take. */
    engine_status(sql);
    if (reader.error) {
        ereport(
            ERROR,
            errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
            errmsg("chdb_search: %s", reader.error)
        );
    }

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

/*
 * The catalog lookups need a transaction; its memory takes the reads too. A
 * database without a store directory has no stores to sweep, and starting an
 * engine for it would only create one.
 */
void
chdb_search_sweep(Oid dboid) {
    char store[MAXPGPATH];
    struct stat st;

    snprintf(store, sizeof(store), CHDB_SEARCH_DIR "/%u", dboid);
    StartTransactionCommand();
    PG_TRY();
    {
        sweep_orphan_dirs(dboid);
        if (stat(store, &st) == 0) {
            sweep_orphan_stores();
        }
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
