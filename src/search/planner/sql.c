/*
 * The scan's statement: the pushed arguments evaluated into the scan keys an
 * index scan would receive and rendered through query.c, the builder the
 * index scan uses, so the two send the same statement. Built at the first
 * row, again with a larger LIMIT when the heap thinned a pushed one, and
 * for EXPLAIN, which has no rows.
 */

#include "postgres.h"

#include "executor/executor.h"
#include "utils/memutils.h"

#include "../query.h"
#include "../search.h"
#include "planner.h"

void
chdb_planner_build_sql(ChdbScanState* st, int64 limit) {
    ExprContext* econtext = st->css.ss.ps.ps_ExprContext;
    int nquals            = list_length(st->spec->quals);
    int norderbys         = list_length(st->spec->orderbys);
    ScanKeyData* keys     = palloc0(sizeof(ScanKeyData) * (nquals + norderbys));
    ListCell *lc, *la = list_head(st->args);
    MemoryContext old;
    int i = 0;

    /* In the per-tuple memory, read before the next row resets it. */
    foreach (lc, list_concat_copy(st->spec->quals, st->spec->orderbys)) {
        ChdbPushed* p = lfirst(lc);
        bool isnull;

        keys[i].sk_argument  = ExecEvalExprSwitchContext(lfirst(la), econtext, &isnull);
        keys[i].sk_flags     = isnull ? SK_ISNULL : 0;
        keys[i].sk_attno     = p->attno;
        keys[i].sk_strategy  = p->strategy;
        keys[i].sk_subtype   = p->subtype;
        keys[i].sk_collation = p->collation;
        la                   = lnext(st->args, la);
        i++;
    }
    old     = MemoryContextSwitchTo(st->cxt);
    st->sql = chdb_search_build_select(
        st->index, keys, nquals, keys + nquals, norderbys, limit
    );
    MemoryContextSwitchTo(old);
    st->built = true;
    st->asked = limit;
}
