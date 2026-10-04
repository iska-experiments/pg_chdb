/*
 * The savepoint marks of the insert buffer. Each buffer keeps a stack of
 * writer checkpoints, one per subtransaction level that has inserted into
 * it, taken before the level's first row. ROLLBACK TO rewinds the writer to
 * the level's checkpoint; RELEASE merges the level into its parent's.
 *
 * A rewind bumps the writer's generation, after which the writer refuses
 * older checkpoints as stale. Those of the enclosing levels are still
 * prefixes of the rewound columns, so they are restamped: without that a
 * second ROLLBACK TO raised inside AbortSubTransaction, which re-entered
 * until ERRORDATA_STACK_SIZE and took the cluster down. The abort path
 * cannot fail, so a rewind that does poisons the buffer instead, and the
 * error is raised at the next insert or at COMMIT.
 */

#include "postgres.h"

#include "access/xact.h"
#include "utils/memutils.h"

#include "buffer.h"

typedef struct Mark {
    SubTransactionId subid;
    pgch_checkpoint ckpt;
} Mark;

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
    chdb_rowwriter_checkpoint(p->rw, &m->ckpt);
    p->marks = lappend(p->marks, m);
    MemoryContextSwitchTo(old);
}

void
chdb_search_free_marks(Pending* p) {
    while (p->marks) {
        pop_mark(p);
    }
}

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
