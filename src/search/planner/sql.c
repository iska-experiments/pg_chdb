/*
 * The scan's statement: the pushed arguments evaluated into the scan keys an
 * index scan would receive and rendered through query.c, the builder the
 * index scan uses, so the two send the same statement; with outputs, the
 * score expressions the store computes, rendered by the access method's
 * score.c from the text searches among the keys and the counts it asks the
 * store for, which the scan keeps for the statement. Built at the first
 * row, again with a larger LIMIT when the heap thinned a pushed one, and
 * for EXPLAIN, which has no rows.
 */

#include "postgres.h"

#include "executor/executor.h"
#include "utils/memutils.h"

#include "../query.h"
#include "../search.h"
#include "planner.h"

/* Whether a search key is NULL, which makes the statement nothing at all. */
static bool
any_null(ScanKey keys, int nkeys) {
    for (int i = 0; i < nkeys; i++) {
        if (keys[i].sk_flags & SK_ISNULL) {
            return true;
        }
    }
    return false;
}

void
chdb_planner_build_sql(ChdbScanState* st, int64 limit) {
    ExprContext* econtext = st->css.ss.ps.ps_ExprContext;
    int nquals            = list_length(st->spec->quals);
    int norderbys         = list_length(st->spec->orderbys);
    int noutputs          = list_length(st->spec->outputs);
    ScanKeyData* keys     = palloc0(sizeof(ScanKeyData) * (nquals + norderbys));
    const char** scores   = palloc0(sizeof(char*) * noutputs);
    ListCell *lc, *la = list_head(st->args);
    MemoryContext old;
    ChdbColumn* cols;
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
    old  = MemoryContextSwitchTo(st->cxt);
    cols = nquals || norderbys ? chdb_search_columns(st->index) : NULL;
    /* No statement with a NULL key: nothing to count for either. */
    i = 0;
    for (lc = list_head(st->spec->outputs); lc && !any_null(keys, nquals);
         lc = lnext(st->spec->outputs, lc)) {
        scores[i++] = chdb_search_score_expr(
            st->index, cols, keys, nquals, ((ChdbOutput*)lfirst(lc))->attno, st->scores
        );
    }
    st->sql = chdb_search_build_scored_select(
        st->index,
        cols,
        keys,
        nquals,
        keys + nquals,
        norderbys,
        scores,
        noutputs,
        st->spec->score_order,
        limit
    );
    MemoryContextSwitchTo(old);
    st->built = true;
    st->asked = limit;
}
