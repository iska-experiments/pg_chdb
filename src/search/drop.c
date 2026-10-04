/*
 * Statements deferred to the end of a transaction: dropping a store when its
 * index is dropped (on commit) or the table its build made when the build
 * rolls back (on abort).
 *
 * The drop is not done where DROP INDEX runs: the transaction can still roll
 * back, and the store would be gone with the index still in the catalog.
 * object_access_hook sees every dropped relation, including those removed by
 * DROP TABLE and DROP SCHEMA ... CASCADE, and records indexes of this access
 * method, and it sees a dropped database, whose per-database directory
 * pg_chdb/<dboid>, and socket file off Linux, it removes at commit. Only
 * backends that loaded chdb_search run the hook, so the library belongs in
 * shared_preload_libraries or session_preload_libraries; a backend that loads
 * it on demand inside a DROP says so in the log. A store the hook missed is
 * swept by the worker when it next starts (sweep.c): the idx_<oid> databases
 * whose OID is no longer a chdb index, and the directories and socket files
 * of databases no longer in pg_database.
 */

#include "postgres.h"

#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_database.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "search.h"
#include "sweep.h"

typedef struct Deferred {
    Oid indexoid;
    char* sql; /* NULL drops the whole store, unless dir is set */
    bool dir;  /* remove the index's database's pg_chdb directory instead */
    bool at_commit;
    SubTransactionId subid;
} Deferred;

static List* deferred                             = NIL; /* in TopTransactionContext */
static object_access_hook_type prev_object_access = NULL;

/*
 * Indexes under DROP INDEX CONCURRENTLY, in TopMemoryContext: the hook fires
 * before index_drop's internal commits, and the store must outlive them.
 * The index is still indisready after the first, so a writer that opened it
 * flushes to it at its own commit, and a cancelled WaitForLockers leaves it
 * cataloged. The drop is deferred at the commit that finds the index gone.
 */
static List* concurrent = NIL;

static void
defer(Oid indexoid, const char* sql, bool at_commit) {
    MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
    Deferred* d       = palloc0(sizeof(*d));

    d->indexoid  = indexoid;
    d->sql       = sql ? pstrdup(sql) : NULL;
    d->at_commit = at_commit;
    d->subid     = GetCurrentSubTransactionId();
    deferred     = lappend(deferred, d);
    MemoryContextSwitchTo(old);
}

/* Defers removal of a dropped database's pg_chdb directory and socket. */
static void
defer_dir(Oid dboid) {
    MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
    Deferred* d       = palloc0(sizeof(*d));

    d->indexoid  = dboid;
    d->dir       = true;
    d->at_commit = true;
    d->subid     = GetCurrentSubTransactionId();
    deferred     = lappend(deferred, d);
    MemoryContextSwitchTo(old);
}

void
chdb_search_drop_on_abort(Oid indexoid) {
    defer(indexoid, NULL, false);
}

void
chdb_search_drop_statement_on_abort(Oid indexoid, const char* sql) {
    defer(indexoid, sql, false);
}

static void
drop_store(Oid indexoid) {
    chdbSearchConn* conn = chdb_search_connect();

    PG_TRY();
    {
        chdb_search_log_sql("drop", psprintf("idx_%u", indexoid));
        chdb_search_drop(conn, indexoid);
    }
    PG_FINALLY();
    { chdb_search_close(conn); }
    PG_END_TRY();
}

/* Past the commit point nothing can be rolled back, so failures only warn. */
static void
run(Deferred* d) {
    if (d->dir) {
        chdb_search_remove_store_dir(d->indexoid);
        return;
    }
    if (d->sql) {
        /* Cleanup of what may never have been made: no generation to check. */
        chdb_search_try_run(d->indexoid, 0, d->sql);
        return;
    }
    PG_TRY();
    { drop_store(d->indexoid); }
    PG_CATCH();
    { chdb_search_warn_failure(d->indexoid); }
    PG_END_TRY();
}

/*
 * PREPARE resets TopTransactionContext, where `deferred` lives, and COMMIT
 * PREPARED runs no callback, so a store drop cannot be carried across it.
 * A rebuild's abort-time statement is merely forgotten: the generation it
 * would have dropped is swept by VACUUM.
 */
static void
pre_prepare(void) {
    ListCell* lc;

    foreach (lc, deferred) {
        if (!((Deferred*)lfirst(lc))->sql) {
            ereport(
                ERROR,
                errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg(
                    "cannot PREPARE a transaction that created or dropped a chdb index"
                )
            );
        }
    }
}

/* Defers the drop of each concurrently dropped index the catalog no longer has. */
static void
settle_concurrent(void) {
    ListCell* lc;

    foreach (lc, concurrent) {
        Oid indexoid = lfirst_oid(lc);

        if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(indexoid))) {
            defer(indexoid, NULL, true);
            concurrent = foreach_delete_current(concurrent, lc);
        }
    }
}

static void
xact_callback(XactEvent event, void* arg) {
    ListCell* lc;

    switch (event) {
    case XACT_EVENT_PRE_PREPARE:
        pre_prepare();
        break;
    case XACT_EVENT_PRE_COMMIT:
        settle_concurrent();
        break;
    case XACT_EVENT_COMMIT:
    case XACT_EVENT_ABORT:
        foreach (lc, deferred) {
            Deferred* d = lfirst(lc);

            if (d->at_commit == (event == XACT_EVENT_COMMIT)) {
                run(d);
            }
        }
        deferred = NIL;
        if (event == XACT_EVENT_ABORT) {
            list_free(concurrent);
            concurrent = NIL;
        }
        break;
    case XACT_EVENT_PREPARE:
    case XACT_EVENT_PARALLEL_COMMIT:
    case XACT_EVENT_PARALLEL_ABORT:
        /* The list's memory goes with the transaction state. */
        deferred = NIL;
        break;
    default:
        break;
    }
}

static void
subxact_callback(
    SubXactEvent event,
    SubTransactionId mySubid,
    SubTransactionId parentSubid,
    void* arg
) {
    ListCell* lc;

    if (event != SUBXACT_EVENT_COMMIT_SUB && event != SUBXACT_EVENT_ABORT_SUB) {
        return;
    }
    foreach (lc, deferred) {
        Deferred* d = lfirst(lc);

        if (d->subid != mySubid) {
            continue;
        }
        if (event == SUBXACT_EVENT_COMMIT_SUB) {
            d->subid = parentSubid;
            continue;
        }

        /* A rolled-back drop keeps its store; a rolled-back build loses it. */
        if (!d->at_commit) {
            run(d);
        }
        deferred = foreach_delete_current(deferred, lc);
    }
}

static void
object_access(
    ObjectAccessType access,
    Oid classId,
    Oid objectId,
    int subId,
    void* arg
) {
    if (prev_object_access) {
        prev_object_access(access, classId, objectId, subId, arg);
    }
    if (access != OAT_DROP || subId != 0) {
        return;
    }
    if (classId == DatabaseRelationId) {
        /* The worker for the database is gone by commit, so its store can go. */
        defer_dir(objectId);
        return;
    }
    if (classId != RelationRelationId || !chdb_search_is_index(objectId)) {
        return;
    }
    if (((ObjectAccessDrop*)arg)->dropflags & PERFORM_DELETION_CONCURRENTLY) {
        MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

        concurrent = list_append_unique_oid(concurrent, objectId);
        MemoryContextSwitchTo(old);
    } else {
        defer(objectId, NULL, true);
    }
}

void
chdb_search_init_drop(void) {
    RegisterXactCallback(xact_callback, NULL);
    RegisterSubXactCallback(subxact_callback, NULL);
    prev_object_access = object_access_hook;
    object_access_hook = object_access;
}
