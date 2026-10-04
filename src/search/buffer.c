/*
 * The per-transaction insert buffer behind aminsert.
 *
 * aminsert never talks to the worker. It appends (ctid, xmin, values) to a
 * per-backend, per-index buffer in TopTransactionContext. At
 * XACT_EVENT_PRE_COMMIT, or XACT_EVENT_PRE_PREPARE, each buffer goes to the
 * worker as Native blocks and the call waits for the acknowledgement, so a
 * committed row is searchable as soon as COMMIT returns. Abort drops the
 * buffers.
 *
 * Subtransactions. A rolled-back savepoint must take its rows with it: each
 * buffer keeps a mark per subtransaction level that has inserted, and
 * marks.c rewinds or merges the level's rows when the savepoint ends.
 *
 * Staging. Past chdb_search.flush_threshold a transaction flushes its
 * buffer into a staging table <table>_tx_<xid> instead, which pre-commit
 * copies into the table and drops, and the abort callback here drops;
 * staging.c does the sending. Rows staged inside a savepoint cannot be
 * rewound, so marks.c remembers their (sub)transaction ids and a rollback
 * excludes them from what the staging table contributes.
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

static void
poisoned_error(Pending* p) {
    ereport(
        ERROR,
        errcode(ERRCODE_INTERNAL_ERROR),
        errmsg("the rows buffered for chdb index %u were lost", p->indexoid),
        errdetail(
            "A savepoint rollback could not rewind them, or a flush failed after "
            "taking them from the buffer."
        ),
        errhint("Roll the transaction back.")
    );
}

/* The transaction's live buffer for the index, or NULL. */
static Pending*
lookup_pending(Oid indexoid) {
    ListCell* lc;

    foreach (lc, pending) {
        Pending* p = lfirst(lc);

        if (p->indexoid == indexoid && p->superseded == InvalidSubTransactionId) {
            return p;
        }
    }
    return NULL;
}

/* For a scan, which could not answer from a buffer whose rows were lost. */
Pending*
chdb_search_pending_of(Oid indexoid) {
    Pending* p = lookup_pending(indexoid);

    if (p && p->poisoned) {
        poisoned_error(p);
    }
    return p;
}

static Pending*
find_pending(Relation index) {
    Pending* p = lookup_pending(RelationGetRelid(index));

    if (p) {
        return p;
    }

    MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

    p             = palloc0(sizeof(*p));
    p->indexoid   = RelationGetRelid(index);
    p->generation = chdb_meta_generation(index);
    p->table      = chdb_search_table_of(p->indexoid, p->generation);
    p->rw         = chdb_rowwriter_new(index);
    p->collist    = chdb_rowwriter_column_list(p->rw);
    pending       = lappend(pending, p);
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
    Pending* p = find_pending(index);

    if (p->poisoned) {
        poisoned_error(p);
    }
    if (GetCurrentTransactionNestLevel() > 1) {
        chdb_search_mark_level(p, GetCurrentSubTransactionId());
    }
    chdb_rowwriter_append(p->rw, ht_ctid, GetCurrentTransactionId(), values, isnull);
    if (chdb_rowwriter_bytes(p->rw) >= (size_t)chdb_search_flush_threshold_kb * 1024) {
        chdb_search_stage_rows(p, index);
    }

    /* The index never reports a uniqueness violation. */
    return false;
}

/*
 * The list and buffers live in TopTransactionContext, which is going away.
 * An abort drops the staging tables first, warning rather than failing past
 * the point where failing would help; what a failed drop leaves, the sweep
 * in vacuum.c removes once the transaction is seen to be over.
 */
static void
reset_pending(bool aborted) {
    ListCell* lc;

    if (aborted) {
        foreach (lc, pending) {
            Pending* p = lfirst(lc);

            if (p->staging) {
                chdb_search_abandon_staging(p);
            }
        }
    }
    pending = NIL;
}

/*
 * Sends one index's rows at commit. The index must first have a store to
 * take them (meta.c): in error mode the refusal aborts the commit; in skip
 * mode the rows stay out and the metapage moves on without them, so only
 * a REINDEX repairs the index. An index dropped later in this transaction
 * has no metapage to check: its rows go to the store that goes with it.
 */
static void
flush_at_commit(Pending* p) {
    Relation index = try_relation_open(p->indexoid, NoLock);
    bool skip      = false;

    if (index) {
        chdb_search_check_available(index, &skip);
    }
    if (skip) {
        if (p->staging) {
            chdb_search_abandon_staging(p);
        }
        chdb_meta_note_skipped(index);
    } else {
        chdb_search_flush_pending(p);
        if (index) {
            chdb_meta_note_flush(index);
        }
    }
    if (index) {
        relation_close(index, NoLock);
    }
}

static void
xact_callback(XactEvent event, void* arg) {
    ListCell* lc;

    switch (event) {
    /*
     * PREPARE flushes as commit does: COMMIT PREPARED and ROLLBACK PREPARED
     * run no callback of ours, so the rows must be in the store before the
     * transaction's fate is decided. A rollback then leaves rows whose heap
     * tuples are dead, as an abort after pre-commit does; the heap fetch
     * hides them and VACUUM removes them.
     */
    case XACT_EVENT_PRE_PREPARE:
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
            flush_at_commit(p);
        }
        break;
    case XACT_EVENT_COMMIT:
    case XACT_EVENT_PREPARE:
        reset_pending(false);
        break;
    case XACT_EVENT_ABORT:
        reset_pending(true);
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
            if (p->staging) {
                chdb_search_abandon_staging(p);
            }
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
