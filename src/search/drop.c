/*
 * Statements deferred to the end of a transaction: dropping a store when its
 * index is dropped (on commit) or its build rolls back (on abort), dropping a
 * staging table on abort.
 *
 * The drop is not done where DROP INDEX runs: the transaction can still roll
 * back, and the store would be gone with the index still in the catalog.
 * object_access_hook sees every dropped relation, including those removed by
 * DROP TABLE and DROP SCHEMA ... CASCADE, and records indexes of this access
 * method. Only backends that loaded chdb_search see it, so the library should
 * be in shared_preload_libraries or session_preload_libraries; a store missed
 * otherwise is swept by the worker, which removes databases idx_<oid> whose
 * OID is not in pg_class.
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "search.h"

typedef struct Deferred {
    Oid indexoid;
    char* sql; /* NULL drops the whole store */
    bool at_commit;
    SubTransactionId subid;
} Deferred;

static List* deferred                             = NIL; /* in TopTransactionContext */
static object_access_hook_type prev_object_access = NULL;

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

void
chdb_search_drop_on_abort(Oid indexoid) {
    defer(indexoid, NULL, false);
}

void
chdb_search_drop_statement_on_abort(Oid indexoid, const char* sql) {
    defer(indexoid, sql, false);
}

/* The statement ran to completion, so there is nothing left to undo. */
void
chdb_search_forget_statement(Oid indexoid, const char* sql) {
    ListCell* lc;

    foreach (lc, deferred) {
        Deferred* d = lfirst(lc);

        if (d->indexoid == indexoid && d->sql && strstr(d->sql, sql)) {
            deferred = foreach_delete_current(deferred, lc);
        }
    }
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
    PG_TRY();
    {
        if (d->sql) {
            chdb_search_run(d->indexoid, d->sql);
        } else {
            drop_store(d->indexoid);
        }
    }
    PG_CATCH();
    {
        ErrorData* e = CopyErrorData();

        FlushErrorState();
        ereport(
            WARNING,
            errmsg(
                "chdb_search: could not clean up index %u: %s", d->indexoid, e->message
            )
        );
        FreeErrorData(e);
    }
    PG_END_TRY();
}

static void
xact_callback(XactEvent event, void* arg) {
    ListCell* lc;

    if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT) {
        return;
    }
    foreach (lc, deferred) {
        Deferred* d = lfirst(lc);

        if (d->at_commit == (event == XACT_EVENT_COMMIT)) {
            run(d);
        }
    }
    deferred = NIL;
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
    if (access != OAT_DROP || classId != RelationRelationId || subId != 0) {
        return;
    }

    HeapTuple tup = SearchSysCache1(RELOID, ObjectIdGetDatum(objectId));

    if (!HeapTupleIsValid(tup)) {
        return;
    }

    Form_pg_class cls = (Form_pg_class)GETSTRUCT(tup);
    bool ours         = cls->relkind == RELKIND_INDEX && OidIsValid(cls->relam) &&
                        cls->relam == get_am_oid("chdb", true);

    ReleaseSysCache(tup);
    if (ours) {
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
