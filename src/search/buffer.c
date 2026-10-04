/*
 * The per-transaction insert buffer behind aminsert.
 *
 * aminsert never talks to the worker. It appends (ctid, xmin, values) to a
 * per-backend, per-index buffer in TopTransactionContext. At
 * XACT_EVENT_PRE_COMMIT each buffer goes to the worker as Native blocks and
 * the call waits for the acknowledgement, so a committed row is searchable as
 * soon as COMMIT returns. Abort drops the buffers.
 *
 * Subtransactions. A rolled-back savepoint must take its rows with it. Each
 * buffer keeps a stack of writer checkpoints, one per subtransaction level
 * that has inserted, taken before the level's first row. ROLLBACK TO rewinds
 * the writer to the checkpoint. Releasing a savepoint merges its level into
 * the parent's.
 *
 * Large transactions. Past chdb_search.flush_threshold a top-level
 * transaction flushes its buffer into a staging table <table>_tx_<xid>
 * instead, which pre-commit copies into the table and drops (abort drops it
 * too); staging.c does the sending. Inside a savepoint nothing is flushed
 * early, because rows already sent could not be taken back.
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

typedef struct Mark {
    SubTransactionId subid;
    pgch_checkpoint ckpt;
} Mark;

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
    p->collist  = chdb_search_column_list(index);
    p->rw       = chdb_rowwriter_new(index);
    pending     = lappend(pending, p);
    MemoryContextSwitchTo(old);
    return p;
}

static Mark*
top_mark(Pending* p) {
    return p->marks ? llast(p->marks) : NULL;
}

static void
pop_mark(Pending* p) {
    Mark* m = llast(p->marks);

    pgch_checkpoint_free(&m->ckpt);
    p->marks = list_delete_last(p->marks);
    pfree(m);
}

static void
free_pending(Pending* p) {
    while (p->marks) {
        pop_mark(p);
    }
    chdb_rowwriter_free(p->rw);
    pfree(p);
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

    if (nested) {
        Mark* top = top_mark(p);

        if (!top || top->subid != subid) {
            Mark* m = palloc0(sizeof(*m));

            m->subid = subid;
            chdb_rowwriter_checkpoint(p->rw, &m->ckpt);
            p->marks = lappend(p->marks, m);
        }
    }
    MemoryContextSwitchTo(old);

    chdb_rowwriter_append(p->rw, ht_ctid, GetCurrentTransactionId(), values, isnull);

    if (!nested &&
        chdb_rowwriter_bytes(p->rw) >= (size_t)chdb_search_flush_threshold_kb * 1024) {
        chdb_search_stage_rows(p);
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

/* Rewinds or merges the level's rows, as the savepoint is rolled back or released. */
static void
settle_marks(Pending* p, SubXactEvent event, SubTransactionId parentSubid) {
    Mark* top = top_mark(p);

    if (event == SUBXACT_EVENT_ABORT_SUB) {
        chdb_rowwriter_rollback(p->rw, &top->ckpt);
        pop_mark(p);
    } else if (parentSubid == TopSubTransactionId) {
        pop_mark(p); /* top level aborts as a whole, no mark needed */
    } else if (
        list_length(p->marks) > 1 &&
        ((Mark*)list_nth(p->marks, list_length(p->marks) - 2))->subid == parentSubid
    ) {
        pop_mark(p); /* the parent's earlier checkpoint already covers these rows */
    } else {
        top->subid = parentSubid;
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
        Mark* top  = top_mark(p);

        if (top && top->subid == mySubid) {
            settle_marks(p, event, parentSubid);
        }
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
