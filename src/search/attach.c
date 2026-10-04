/*
 * Attaching an index's store table on demand. The worker wipes the
 * engine's directory when it starts (sweep.c), since what the engine's
 * metadata said about tables, parts and generations could be older or newer
 * than the index relation it served after a crash, a restore or a
 * promotion; so the engine starts with no tables, and the first request
 * for a generation puts its table back with the statement that created it
 * (ddl.c), whose fixed UUID lets the disk find the parts under the key
 * prefix. The index is opened without a lock: the backend asking holds one,
 * and the worker taking another would wait behind a REINDEX waiting for the
 * worker. A build in progress sends its own CREATE TABLE and needs none of
 * this; its relation is in no catalog the worker can read anyway.
 */

#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "attach.h"
#include "engine_proc.h"
#include "frame.h"
#include "pagestore/pagestore.h"
#include "protocol.h"
#include "search.h"
#include "sweep.h"

typedef struct Attached {
    Oid index;
    uint64 generation; /* the last one attached for the index */
} Attached;

/* By index, what this worker has attached, which the engine keeps until replaced. */
static HTAB* attached;

static Attached*
attached_entry(Oid index, bool enter) {
    if (!attached) {
        HASHCTL ctl = {
            .keysize   = sizeof(Oid),
            .entrysize = sizeof(Attached),
            .hcxt      = TopMemoryContext,
        };

        attached = hash_create(
            "chdb_search attached tables",
            64,
            &ctl,
            HASH_ELEM | HASH_BLOBS | HASH_CONTEXT
        );
    }
    return hash_search(attached, &index, enter ? HASH_ENTER : HASH_FIND, NULL);
}

/* Whether the storage holds any blob under `prefix`. */
static void
count_sink(void* ud, const char* key, uint64 size, int64 mtime) {
    (*(int*)ud)++;
}

static bool
has_blobs(Oid index, const char* prefix) {
    int n = 0;

    chdb_pagestore_list(psprintf(CHDB_STORE_STORAGE_FMT, index), prefix, count_sink, &n);
    return n > 0;
}

/* The transactions with staging tables in the generation's blobs, by key prefix. */
static void
staging_sink(void* ud, const char* key, uint64 size, int64 mtime) {
    List** fxids    = ud;
    const char* tx  = strstr(key, "_tx_");
    uint64 fxid     = tx ? strtoull(tx + 4, NULL, 10) : 0;

    if (fxid) {
        ListCell* lc;

        foreach (lc, *fxids) {
            if (*(uint64*)lfirst(lc) == fxid) {
                return;
            }
        }

        uint64* v = palloc(sizeof(*v));

        *v     = fxid;
        *fxids = lappend(*fxids, v);
    }
}

/*
 * The attach statements for the index's table and the staging tables of
 * its generation, if the catalog has the index, its metapage names
 * `*generation`, or any generation when zero is asked, which is then set,
 * and the table has been made: MergeTree writes a file into a table's
 * directory as it creates it, so a generation without a blob is one whose
 * CREATE TABLE is still to come, as a CREATE INDEX CONCURRENTLY's is once
 * its catalog entry is in, and attaching would make an empty table in its
 * way. Palloc'd in the caller's context. A staging table is known by the
 * blobs under its key prefix: of a transaction still running, which goes
 * on writing it, or of one that is over, which VACUUM sweeps.
 */
static List*
attach_sqls(Oid index, uint64* generation) {
    MemoryContext caller = CurrentMemoryContext;
    List* sqls           = NIL;
    List* fxids          = NIL;
    ListCell* lc;

    StartTransactionCommand();

    Relation rel = try_relation_open(index, NoLock);

    if (rel) {
        uint64 current = chdb_search_is_index(index) ? chdb_meta_generation(rel) : 0;

        if (current && (*generation == 0 || *generation == current) &&
            has_blobs(index, psprintf(CHDB_STORE_KEY_PREFIX_FMT "/", current))) {
            MemoryContext old = MemoryContextSwitchTo(caller);

            *generation = current;
            sqls        = lappend(sqls, chdb_search_attach_sql(rel, 0));
            chdb_pagestore_list(
                psprintf(CHDB_STORE_STORAGE_FMT, index),
                psprintf("s" UINT64_FORMAT "_tx_", current),
                staging_sink,
                &fxids
            );
            foreach (lc, fxids) {
                sqls = lappend(sqls, chdb_search_attach_sql(rel, *(uint64*)lfirst(lc)));
            }
            MemoryContextSwitchTo(old);
        }
        relation_close(rel, NoLock);
    }
    CommitTransactionCommand();
    MemoryContextSwitchTo(caller);
    return sqls;
}

/* Runs a statement on the engine as a backend would, raising its error. */
static void
engine_exec(Oid index, const char* sql) {
    StringInfoData buf;
    char* dead = engine_ensure(MyDatabaseId);
    uint8_t status;

    if (dead) {
        ereport(ERROR, errmsg("chdb_search: %s", dead));
    }
    initStringInfo(&buf);
    chdb_search_frame_request(&buf, CHDB_CMD_EXEC, index, NULL, 0, sql);
    if (!engine_send(buf.data, buf.len)) {
        ereport(ERROR, errmsg("chdb_search: %s", engine_death()));
    }
    pfree(buf.data);

    char* text =
        chdb_search_frame_status(engine_channel(), "chDB engine", sql, &status);

    if (status != CHDB_STATUS_OK) {
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
 * A request naming the generation last attached for its index costs nothing
 * here; one naming none, as VACUUM's sweep and a store's DDL do, or another
 * generation, as the first after a REINDEX does, asks the catalog.
 */
void
chdb_search_attach(Oid index, uint64 generation) {
    Attached* a = OidIsValid(index) ? attached_entry(index, false) : NULL;
    ListCell* lc;

    if (!OidIsValid(index) || (a && generation != 0 && a->generation == generation)) {
        return;
    }

    List* sqls = attach_sqls(index, &generation);

    if (a && a->generation == generation) {
        return; /* a request naming no generation, for the one attached */
    }
    foreach (lc, sqls) {
        chdb_search_log_sql("attach", lfirst(lc));
        engine_exec(index, lfirst(lc));
    }
    if (sqls) {
        attached_entry(index, true)->generation = generation;
    }
}
