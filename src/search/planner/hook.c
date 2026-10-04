/*
 * The set_rel_pathlist hook: for a heap relation with a chdb index, a
 * CustomPath that sends the pushable part of the query to the store as one
 * statement, when there is a search predicate to send or an order the
 * store can serve. The index scan the planner made already competes with
 * it; cost.c prices the custom scan below it for the same work, and
 * chdb_search.enable_custom_scan turns it off altogether.
 *
 * The LIMIT goes along when the whole query is the scan: one relation, an
 * ORDER BY the path satisfies entirely, every predicate pushed, and nothing
 * between the scan and the LIMIT that changes the row count (grouping,
 * DISTINCT, window functions, set-returning functions, row locks). Rows the
 * heap fetch then hides, deleted or updated since the store took them, are
 * made up for at execution by asking again for more (exec.c).
 *
 * A query that calls chdb.score() on the relation, with a text search to
 * score, has the store compute it (score.c): the path takes the calls as
 * its outputs and, as the other paths would evaluate the placeholder and
 * raise, replaces them.
 */

#include "postgres.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "catalog/pg_am_d.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include "../../vector/chdb_vector.h"
#include "../query.h"
#include "../search.h"
#include "../vector.h"
#include "planner.h"

bool chdb_search_enable_custom_scan         = true;
double chdb_search_custom_scan_cost_factor  = 0.5;
static set_rel_pathlist_hook_type prev_hook = NULL;

/* The index's access method is ours: told by address, as columns.c tells kinds. */
static bool
is_chdb_index(IndexOptInfo* index) {
    return index->amcostestimate == chdb_search_costestimate && !index->hypothetical &&
           (index->indpred == NIL || index->predOK);
}

/*
 * An unavailable store in skip mode gets no path, as the index gets no
 * usable one; in error mode the scan raises, as the index scan would.
 */
bool
chdb_planner_usable_index(IndexOptInfo* index) {
    bool unavailable = false;

    if (!is_chdb_index(index)) {
        return false;
    }
    if (chdb_search_unavailable_index == CHDB_UNAVAILABLE_SKIP) {
        Relation rel_index = index_open(index->indexoid, NoLock);

        unavailable = chdb_search_store_unavailable(rel_index);
        index_close(rel_index, NoLock);
    }
    return !unavailable;
}

/* A plain heap table, so that its tuples fit the scan's slot. */
bool
chdb_planner_eligible_rel(RelOptInfo* rel, RangeTblEntry* rte) {
    return rel->reloptkind == RELOPT_BASEREL && rte->rtekind == RTE_RELATION &&
           rel->indexlist != NIL && get_rel_relam(rte->relid) == HEAP_TABLE_AM_OID;
}

/*
 * Whether the LIMIT would cut a filtered vector search short. ClickHouse's
 * HNSW index finds the LIMIT nearest rows first and applies the WHERE to
 * those, unless chdb_vector.filter_strategy says prefilter, so a filtered
 * search with a small LIMIT returns fewer rows than match: it asks for as
 * many rows as the index serves instead, as the index scan does, and the
 * LIMIT stays with Postgres.
 */
static bool
postfiltered(ChdbPath* p) {
    const char* strategy;
    Relation index;
    bool vector = false;
    ListCell* lc;

    if (p->spec.quals == NIL) {
        return false;
    }
    index = index_open(p->spec.indexoid, NoLock);
    foreach (lc, p->spec.orderbys) {
        vector |= chdb_search_is_vector(index, ((ChdbPushed*)lfirst(lc))->attno);
    }
    index_close(index, NoLock);
    if (!vector) {
        return false;
    }
    strategy = GetConfigOption(CHDB_VECTOR_GUC_FILTER, true, false);
    return !strategy || strcmp(strategy, "prefilter") != 0;
}

/*
 * Whether a LIMIT of `root->limit_tuples` rows applies to this scan's rows
 * as the store would count them, and so can be sent along.
 */
static bool
limit_applies(PlannerInfo* root, RelOptInfo* rel) {
    Query* parse = root->parse;

    return root->limit_tuples > 0 && root->limit_tuples <= INT_MAX &&
           bms_equal(root->all_baserels, rel->relids) && parse->groupClause == NIL &&
           parse->groupingSets == NIL && !parse->hasAggs && !parse->hasWindowFuncs &&
           parse->distinctClause == NIL && !parse->hasTargetSRFs &&
           parse->rowMarks == NIL && parse->havingQual == NULL &&
           compare_pathkeys(root->query_pathkeys, root->sort_pathkeys) ==
               PATHKEYS_EQUAL;
}

/* By index column, as the index scan orders its keys: the same statement. */
static int
by_column(const ListCell* a, const ListCell* b) {
    return ((const ChdbPushed*)lfirst(a))->attno -
           ((const ChdbPushed*)lfirst(b))->attno;
}

bool
chdb_planner_collect_quals(PlannerInfo* root, RelOptInfo* rel, ChdbPath* p) {
    bool search = false;
    ListCell* lc;

    foreach (lc, rel->baserestrictinfo) {
        RestrictInfo* rinfo = lfirst(lc);
        ChdbPushed* pushed  = palloc0(sizeof(*pushed));

        if (chdb_planner_match_clause(root, rel, p->index, rinfo, pushed)) {
            p->spec.quals = lappend(p->spec.quals, pushed);
            search |= pushed->strategy < CHDB_STRATEGY_EQ;
        } else {
            p->local = lappend(p->local, rinfo);
            pfree(pushed);
        }
    }
    list_sort(p->spec.quals, by_column);
    return search;
}

/* The query's ORDER BY as pushed expressions, if the store can serve all of it. */
static bool
collect_orderbys(PlannerInfo* root, RelOptInfo* rel, ChdbPath* p) {
    ListCell* lc;

    if (root->query_pathkeys == NIL) {
        return false;
    }
    /* By the score alone, descending: the store breaks the ties by ctid. */
    if (list_length(root->query_pathkeys) == 1 &&
        chdb_planner_match_score_pathkey(
            linitial(root->query_pathkeys), p->spec.outputs, &p->spec.score_order
        )) {
        return true;
    }
    foreach (lc, root->query_pathkeys) {
        ChdbPushed* pushed = palloc0(sizeof(*pushed));

        if (!chdb_planner_match_pathkey(root, rel, p->index, lfirst(lc), pushed)) {
            list_free_deep(p->spec.orderbys);
            p->spec.orderbys = NIL;
            return false;
        }
        p->spec.orderbys = lappend(p->spec.orderbys, pushed);
    }
    return true;
}

/*
 * Leaves the relation the chdb custom scans that compute the score alone:
 * every other path, another chdb index's included, would evaluate
 * chdb.score() in Postgres, where it raises. The hook runs after the core
 * planner added its paths and before the partial ones are gathered.
 */
static void
drop_other_paths(RelOptInfo* rel) {
    List* keep = NIL;
    ListCell* lc;

    foreach (lc, rel->pathlist) {
        Path* path = lfirst(lc);

        if (IsA(path, CustomPath) &&
            ((CustomPath*)path)->methods == &chdb_planner_path_methods &&
            ((ChdbPath*)path)->spec.outputs != NIL) {
            keep = lappend(keep, path);
        }
    }
    rel->pathlist         = keep;
    rel->partial_pathlist = NIL;
}

static void
add_scan_path(PlannerInfo* root, RelOptInfo* rel, IndexOptInfo* index) {
    ChdbPath* p = palloc0(sizeof(*p));
    Path* path  = &p->cpath.path;
    bool search, ordered;

    p->index         = index;
    p->spec.indexoid = index->indexoid;
    p->spec.limit    = -1;
    search           = chdb_planner_collect_quals(root, rel, p);
    /* A score is of the text searches, so there must be one to score. */
    if (search) {
        p->spec.outputs = chdb_planner_collect_scores(root, rel, index);
    }
    ordered = collect_orderbys(root, rel, p);
    if (!search && !ordered) {
        pfree(p);
        return;
    }
    if (ordered && p->local == NIL && limit_applies(root, rel) && !postfiltered(p)) {
        p->spec.limit = (int64)ceil(root->limit_tuples);
    }

    path->type             = T_CustomPath;
    path->pathtype         = T_CustomScan;
    path->parent           = rel;
    path->pathtarget       = rel->reltarget;
    path->param_info       = NULL;
    path->parallel_aware   = false;
    path->parallel_safe    = false;
    path->parallel_workers = 0;
    path->pathkeys         = ordered ? root->query_pathkeys : NIL;
    p->cpath.flags         = CUSTOMPATH_SUPPORT_PROJECTION; /* the heap tuple */
    p->cpath.custom_paths  = NIL;
    p->cpath.methods       = &chdb_planner_path_methods;
    chdb_planner_cost(root, rel, p);
    if (p->spec.outputs) {
        drop_other_paths(rel);
    }
    add_path(rel, path);
}

static void
rel_pathlist_hook(PlannerInfo* root, RelOptInfo* rel, Index rti, RangeTblEntry* rte) {
    ListCell* lc;

    if (prev_hook) {
        prev_hook(root, rel, rti, rte);
    }
    if (!chdb_search_enable_custom_scan || !chdb_planner_eligible_rel(rel, rte)) {
        return;
    }
    foreach (lc, rel->indexlist) {
        if (chdb_planner_usable_index(lfirst(lc))) {
            add_scan_path(root, rel, lfirst(lc));
        }
    }
}

void
chdb_search_planner_init(void) {
    DefineCustomBoolVariable(
        "chdb_search.enable_custom_scan",
        "Plan a custom scan that sends a search to the store as one statement.",
        "Off leaves the chdb index scan, which sends the same statement through "
        "the access method.",
        &chdb_search_enable_custom_scan,
        true,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    DefineCustomRealVariable(
        "chdb_search.custom_scan_cost_factor",
        "Multiplier on the estimated run cost of a chdb custom scan.",
        "Below 1 the planner prefers the custom scan to an index scan of the same "
        "index for the same rows; above 1 the index scan.",
        &chdb_search_custom_scan_cost_factor,
        0.5,
        0.0,
        1000.0,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    prev_hook             = set_rel_pathlist_hook;
    set_rel_pathlist_hook = rel_pathlist_hook;
}
