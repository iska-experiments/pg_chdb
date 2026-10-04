/*
 * The per-transaction insert buffer behind aminsert.
 *
 * aminsert never talks to the worker. It appends (ctid, xmin, values) to a
 * per-backend, per-index buffer in TopTransactionContext. At
 * XACT_EVENT_PRE_COMMIT each buffer goes to the worker as Native blocks and
 * the call waits for the acknowledgement, so a committed row is searchable as
 * soon as COMMIT returns. Abort drops the buffers.
 *
 * Subtransactions. A rolled-back savepoint must take its rows with it: each
 * buffer keeps a mark per subtransaction level that has inserted, and
 * marks.c rewinds or merges the level's rows when the savepoint ends.
 *
 * Large transactions. Past chdb_search.flush_threshold a top-level
 * transaction flushes its buffer into a staging table <table>_tx_<xid>
 * instead, which pre-commit copies into the table and drops (abort drops it
 * too); staging.c does the sending. Inside a savepoint nothing is flushed
 * early, because rows already sent could not be taken back, so the buffer
 * grows until COMMIT: it warns once past the threshold and fails past
 * chdb_search.max_buffer, since an error inside the savepoint can be caught
 * and an OOM kill cannot.
 *
 * Rebuilds. A REINDEX or TRUNCATE in the same transaction indexes the
 * transaction's own tuples in its build scan and names a new table, so the
 * rows buffered so far must not be sent again and later rows need a buffer
 * for the new table. The old buffer is discarded, or, inside a savepoint
 * that may yet roll the rebuild back, set aside until that is settled.
 */

#include "postgres.h"

#include "access/relation.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "pg-clickhouse-encode.h"

#include "buffer.h"
#include "search.h"

/* ---- per-transaction buffers ---- */

static List* pending = NIL; /* of Pending*, in TopTransactionContext */

static Pending*
find_pending(Relation index) {
    ListCell* lc;

    foreach (lc, pending) {
        Pending* p = lfirst(lc);

        if (p->indexoid == RelationGetRelid(index) &&
            p->superseded == InvalidSubTransactionId) {
            return p;
        }
    }

    MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
    Pending* p        = palloc0(sizeof(*p));

    p->indexoid = RelationGetRelid(index);
    p->table    = chdb_search_table_name(index);
    p->rw       = chdb_rowwriter_new(index);
    p->collist  = chdb_rowwriter_column_list(p->rw);
    pending     = lappend(pending, p);
    MemoryContextSwitchTo(old);
    return p;
}

static void
free_pending(Pending* p) {
    chdb_search_free_marks(p);
    if (p->rw) {
        chdb_rowwriter_free(p->rw);
    }
    pfree(p);
}

static void
poisoned_error(Pending* p) {
    ereport(
        ERROR,
        errcode(ERRCODE_INTERNAL_ERROR),
        errmsg(
            "the rows buffered for chdb index %u were lost in a savepoint rollback",
            p->indexoid
        ),
        errhint("Roll the transaction back.")
    );
}

/*
 * Called by a rebuild of the index. Its build scan covers the transaction's
 * own tuples, so the rows buffered so far would reach the store twice, and
 * a staged copy belongs to the generation being replaced. At the top level
 * nothing can bring the rebuild back, so the buffer goes; inside a savepoint
 * it is set aside until the savepoint is released or rolled back.
 */
void
chdb_search_discard_pending(Oid indexoid) {
    ListCell* lc;

    foreach (lc, pending) {
        Pending* p = lfirst(lc);

        if (p->indexoid != indexoid || p->superseded != InvalidSubTransactionId) {
            continue;
        }
        if (GetCurrentTransactionNestLevel() > 1) {
            p->superseded = GetCurrentSubTransactionId();
        } else {
            if (p->staging) {
                chdb_search_drop_staging(p);
            }
            free_pending(p);
            pending = foreach_delete_current(pending, lc);
        }
    }
}

bool
chdb_search_aminsert(
    Relation index,
    Datum* values,
    bool* isnull,
    ItemPointer ht_ctid,
    Relation heap,
    IndexUniqueCheck checkUnique,
    bool indexUnchanged,
    struct IndexInfo* indexInfo
) {
    Pending* p             = find_pending(index);
    SubTransactionId subid = GetCurrentSubTransactionId();
    bool nested            = GetCurrentTransactionNestLevel() > 1;
    MemoryContext old      = MemoryContextSwitchTo(TopTransactionContext);

    if (p->poisoned) {
        poisoned_error(p);
    }
    if (nested) {
        chdb_search_mark_level(p, subid);
    }
    MemoryContextSwitchTo(old);

    chdb_rowwriter_append(p->rw, ht_ctid, GetCurrentTransactionId(), values, isnull);

    size_t bytes = chdb_rowwriter_bytes(p->rw);

    if (!nested && bytes >= (size_t)chdb_search_flush_threshold_kb * 1024) {
        chdb_search_stage_rows(p);
    } else if (
        chdb_search_max_buffer_kb && bytes >= (size_t)chdb_search_max_buffer_kb * 1024
    ) {
        ereport(
            ERROR,
            errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
            errmsg(
                "chdb index \"%s\" buffer exceeds chdb_search.max_buffer",
                RelationGetRelationName(index)
            ),
            errhint("Insert outside a savepoint, or raise the setting.")
        );
    } else if (
        nested && !p->warned && bytes >= (size_t)chdb_search_flush_threshold_kb * 1024
    ) {
        p->warned = true;
        ereport(
            WARNING,
            errmsg(
                "chdb index \"%s\" buffers rows inserted inside a savepoint until "
                "COMMIT",
                RelationGetRelationName(index)
            ),
            errdetail(
                "The buffer has passed chdb_search.flush_threshold and grows until the "
                "transaction ends, up to chdb_search.max_buffer."
            )
        );
    }

    /* The index never reports a uniqueness violation. */
    return false;
}

static void
reset_pending(void) {
    /* The list and buffers live in TopTransactionContext, which is going away. */
    pending = NIL;
}

static void
xact_callback(XactEvent event, void* arg) {
    ListCell* lc;

    switch (event) {
    case XACT_EVENT_PRE_PREPARE:
        foreach (lc, pending) {
            Pending* p = lfirst(lc);

            if (chdb_rowwriter_rows(p->rw) || p->staging) {
                ereport(
                    ERROR,
                    errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("cannot PREPARE a transaction that changed a chdb index")
                );
            }
        }
        break;
    case XACT_EVENT_PRE_COMMIT:
        foreach (lc, pending) {
            Pending* p = lfirst(lc);

            if (p->superseded != InvalidSubTransactionId) {
                /* A rebuild took these rows; its staged copy goes with them. */
                if (p->staging) {
                    chdb_search_drop_staging(p);
                }
                continue;
            }
            if (p->poisoned) {
                poisoned_error(p);
            }
            if (chdb_rowwriter_rows(p->rw) == 0 && !p->staging) {
                continue;
            }
            chdb_search_flush_pending(p);

            /* The index may have been dropped later in this transaction. */
            Relation index = try_relation_open(p->indexoid, NoLock);

            if (index) {
                chdb_meta_note_flush(index);
                relation_close(index, NoLock);
            }
        }
        break;
    case XACT_EVENT_COMMIT:
    case XACT_EVENT_ABORT:
    case XACT_EVENT_PREPARE:
        reset_pending();
        break;
    default:
        break;
    }
}

static bool
revived_for(List* revived, Oid indexoid) {
    ListCell* lc;

    foreach (lc, revived) {
        if (((Pending*)lfirst(lc))->indexoid == indexoid) {
            return true;
        }
    }
    return false;
}

static void
subxact_callback(
    SubXactEvent event,
    SubTransactionId mySubid,
    SubTransactionId parentSubid,
    void* arg
) {
    List* revived = NIL;
    ListCell* lc;

    if (event != SUBXACT_EVENT_COMMIT_SUB && event != SUBXACT_EVENT_ABORT_SUB) {
        return;
    }

    foreach (lc, pending) {
        Pending* p = lfirst(lc);

        chdb_search_settle_marks(p, event, mySubid, parentSubid);
        if (p->superseded == mySubid) {
            /* The rebuild that set the rows aside is undone with the savepoint,
             * or passes to the parent with it. */
            if (event == SUBXACT_EVENT_ABORT_SUB) {
                p->superseded = InvalidSubTransactionId;
                revived       = lappend(revived, p);
            } else {
                p->superseded = parentSubid;
            }
        }
    }

    /* Rows buffered for the undone rebuild's table were all inserted since it. */
    foreach (lc, pending) {
        Pending* p = lfirst(lc);

        if (p->superseded == InvalidSubTransactionId && !list_member_ptr(revived, p) &&
            revived_for(revived, p->indexoid)) {
            free_pending(p);
            pending = foreach_delete_current(pending, lc);
        }
    }
    list_free(revived);
}

void
chdb_search_init_insert(void) {
    RegisterXactCallback(xact_callback, NULL);
    RegisterSubXactCallback(subxact_callback, NULL);
}
