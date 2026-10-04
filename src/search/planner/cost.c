/*
 * What a custom scan costs, priced as cost_index prices an index scan of
 * the same index minus the index's own share: the store answers in one
 * round trip, so the startup is a random page, and each row it returns is
 * a heap fetch, as it is for the index scan. With the whole query pushed
 * the two do the same work and the custom scan skips the access method's
 * per-tuple overhead, which chdb_search.custom_scan_cost_factor stands for:
 * below 1 the custom scan wins, above 1 the index scan.
 */

#include "postgres.h"

#include "optimizer/cost.h"
#include "optimizer/optimizer.h"

#include "planner.h"

/* The RestrictInfos of the pushed predicates, for their selectivity. */
static List*
pushed_rinfos(const ChdbPath* p) {
    List* rinfos = NIL;
    ListCell* lc;

    foreach (lc, p->spec.quals) {
        rinfos = lappend(rinfos, ((ChdbPushed*)lfirst(lc))->rinfo);
    }
    return rinfos;
}

void
chdb_planner_cost(PlannerInfo* root, RelOptInfo* rel, ChdbPath* p) {
    Path* path = &p->cpath.path;
    QualCost local_cost;
    double store_rows, pages;
    Cost startup, run;

    /*
     * Rows the store would return without a LIMIT: every row the pushed
     * predicates pass, which is rel->rows when they are all the predicates.
     */
    if (p->local == NIL) {
        store_rows = rel->rows;
    } else {
        store_rows = clamp_row_est(
            rel->tuples *
            clauselist_selectivity(root, pushed_rinfos(p), rel->relid, JOIN_INNER, NULL)
        );
    }
    path->rows = p->local == NIL ? store_rows : rel->rows;

    /* The round trip, then a heap page per row until the heap is read. */
    startup = random_page_cost;
    pages = index_pages_fetched(store_rows, rel->pages, (double)p->index->pages, root);
    run   = pages * random_page_cost;

    /* Each row: the fetch, the clauses kept here, the target list. */
    cost_qual_eval(&local_cost, p->local, root);
    startup += local_cost.startup;
    run += store_rows * (cpu_tuple_cost + local_cost.per_tuple);
    run += path->rows * path->pathtarget->cost.per_tuple;
    startup += path->pathtarget->cost.startup;

    /*
     * A pushed LIMIT stops the scan early, as a Limit node would stop the
     * index scan: the same fraction of the run cost, so that the two stay
     * comparable and the factor decides.
     */
    if (p->spec.limit >= 0 && (double)p->spec.limit < store_rows) {
        run *= (double)p->spec.limit / store_rows;
        path->rows = (double)p->spec.limit;
    }

    path->startup_cost = startup;
    path->total_cost   = startup + run * chdb_search_custom_scan_cost_factor;
}
