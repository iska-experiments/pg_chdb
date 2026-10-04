/*
 * The savepoint marks of the insert buffer. Each buffer keeps a stack of
 * writer checkpoints, one per subtransaction level that has inserted into
 * it, taken before the level's first row. ROLLBACK TO rewinds the writer to
 * the level's checkpoint; RELEASE merges the level into its parent's.
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

void
chdb_search_settle_marks(
    Pending* p,
    SubXactEvent event,
    SubTransactionId mySubid,
    SubTransactionId parentSubid
) {
    Mark* top = top_mark(p);

    if (!top || top->subid != mySubid) {
        return;
    }
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
