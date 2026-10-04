/*
 * The savepoint bookkeeping of the insert buffer.
 *
 * Marks. Each buffer keeps a stack of writer checkpoints, one per
 * subtransaction level that has inserted into it, taken before the level's
 * first row. ROLLBACK TO rewinds the writer to the level's checkpoint;
 * RELEASE merges the level into its parent's.
 *
 * A rewind bumps the writer's generation, after which the writer refuses
 * older checkpoints as stale. Those of the enclosing levels are still
 * prefixes of the rewound columns, so they are restamped: without that a
 * second ROLLBACK TO raised inside AbortSubTransaction, which re-entered
 * until ERRORDATA_STACK_SIZE and took the cluster down. The abort path
 * cannot fail, so a rewind that does poisons the buffer instead, and the
 * error is raised at the next insert or at COMMIT.
 *
 * Staged levels. Shipping the buffer to the staging table (staging.c)
 * empties the writer and so invalidates every mark, which then becomes a
 * staged level: the subtransaction and its transaction id, which every row
 * it inserted carries in the xmin column. Rows already in the store cannot
 * be taken back by a rewind, so ROLLBACK TO of a staged level puts its id on
 * the excluded list, by which scans and the commit filter the staging table
 * (`xmin NOT IN (...)`). RELEASE passes a staged level to its parent, or
 * forgets it at the top level, which never rolls back on its own: an abort
 * drops the whole staging table. The top level itself needs no mark and no
 * staged entry for the same reason.
 */

#include "postgres.h"

#include "access/xact.h"
#include "utils/memutils.h"

#include "buffer.h"

typedef struct Mark {
    SubTransactionId subid;
    TransactionId xid; /* the level's, as its rows' xmin */
    pgch_checkpoint ckpt;
} Mark;

typedef struct Staged {
    SubTransactionId subid;
    TransactionId xid;
} Staged;

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

void
chdb_search_mark_level(Pending* p, SubTransactionId subid) {
    Mark* top = top_mark(p);

    if (top && top->subid == subid) {
        return;
    }

    MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
    Mark* m           = palloc0(sizeof(*m));

    m->subid = subid;
    m->xid   = GetCurrentTransactionId();
    chdb_rowwriter_checkpoint(p->rw, &m->ckpt);
    p->marks = lappend(p->marks, m);
    MemoryContextSwitchTo(old);
}

void
chdb_search_free_marks(Pending* p) {
    while (p->marks) {
        pop_mark(p);
    }
    list_free_deep(p->staged);
    list_free(p->excluded);
    p->staged = p->excluded = NIL;
}

/* ---- staged levels ---- */

/* Records the level as staged, once. */
static void
add_staged(Pending* p, SubTransactionId subid, TransactionId xid) {
    ListCell* lc;

    foreach (lc, p->staged) {
        Staged* s = lfirst(lc);

        if (s->subid == subid && s->xid == xid) {
            return;
        }
    }

    MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
    Staged* s         = palloc(sizeof(*s));

    s->subid  = subid;
    s->xid    = xid;
    p->staged = lappend(p->staged, s);
    MemoryContextSwitchTo(old);
}

void
chdb_search_marks_shipped(Pending* p) {
    while (p->marks) {
        Mark* m = top_mark(p);

        add_staged(p, m->subid, m->xid);
        pop_mark(p);
    }
}

char*
chdb_search_excluded_xids(Pending* p) {
    StringInfoData buf;
    ListCell* lc;

    if (!p->excluded) {
        return NULL;
    }
    initStringInfo(&buf);
    foreach (lc, p->excluded) {
        appendStringInfo(
            &buf, "%s%u", buf.len ? ", " : "", (TransactionId)lfirst_int(lc)
        );
    }
    return buf.data;
}

/* Excludes the aborted level's staged rows, or passes them to the parent. */
static void
settle_staged(
    Pending* p,
    SubXactEvent event,
    SubTransactionId mySubid,
    SubTransactionId parentSubid
) {
    MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
    ListCell* lc;

    foreach (lc, p->staged) {
        Staged* s = lfirst(lc);

        if (s->subid != mySubid) {
            continue;
        }
        if (event == SUBXACT_EVENT_ABORT_SUB) {
            p->excluded = list_append_unique_int(p->excluded, (int)s->xid);
        } else if (parentSubid != TopSubTransactionId) {
            s->subid = parentSubid;
            continue;
        }
        pfree(s);
        p->staged = foreach_delete_current(p->staged, lc);
    }
    MemoryContextSwitchTo(old);
}

/* ---- settling a savepoint ---- */

/* Rewinds the buffer to the aborted level's checkpoint, or poisons it. */
static void
rewind_marks(Pending* p) {
    Mark* top = top_mark(p);
    ListCell* lc;

    PG_TRY();
    { chdb_rowwriter_rollback(p->rw, &top->ckpt); }
    PG_CATCH();
    {
        FlushErrorState();
        chdb_rowwriter_free(p->rw);
        p->rw       = NULL;
        p->poisoned = true;
    }
    PG_END_TRY();
    pop_mark(p);
    if (p->poisoned) {
        return;
    }
    foreach (lc, p->marks) {
        chdb_rowwriter_revalidate(p->rw, &((Mark*)lfirst(lc))->ckpt);
    }
}

void
chdb_search_settle_marks(
    Pending* p,
    SubXactEvent event,
    SubTransactionId mySubid,
    SubTransactionId parentSubid
) {
    Mark* top = top_mark(p);

    settle_staged(p, event, mySubid, parentSubid);
    if (!top || top->subid != mySubid || p->poisoned) {
        return;
    }
    if (event == SUBXACT_EVENT_ABORT_SUB) {
        rewind_marks(p);
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
