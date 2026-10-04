/*
 * The create_upper_paths hook: a GROUP BY or aggregate query over one heap
 * relation with a chdb index, every clause of which the store applies, is
 * answered by the store as one statement, `SELECT <groups>, <aggregates>
 * FROM t WHERE ... GROUP BY ...`, when the heap vouches for the answer.
 *
 * MVCC. The store holds a row per heap tuple from committed transactions
 * until VACUUM removes the dead ones, and knows nothing of the query's
 * snapshot, so its count is the heap's only when every heap page is
 * all-visible: no dead tuple is left for VACUUM and no tuple is from a
 * transaction the snapshot does not see. That is what the visibility map
 * says, and the same rule an index-only scan goes by. The path is made only
 * when the map says so at planning; the executor (agg_exec.c) asks again
 * before the statement and after its answer, and when the heap changed in
 * between runs the exact plan instead, the Agg over the planner's cheapest
 * scan that the path carries as its child, so the answer is always right.
 *
 * chdb_search.enable_aggregate_pushdown turns it off; it also needs the
 * custom scan enabled, being one.
 */

#include "postgres.h"

#include "access/table.h"
#include "access/visibilitymap.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "utils/guc.h"
#include "utils/selfuncs.h"

#include "../search.h"
#include "planner.h"

bool chdb_search_enable_aggregate_pushdown    = true;
static create_upper_paths_hook_type prev_hook = NULL;

bool
chdb_planner_heap_all_visible(Relation heap) {
    BlockNumber all_visible, all_frozen;

    visibilitymap_count(heap, &all_visible, &all_frozen);
    return all_visible == RelationGetNumberOfBlocks(heap);
}

/* The grouping columns' expressions, for the group estimate. */
static List*
group_exprs(PlannerInfo* root) {
    return get_sortgrouplist_exprs(root->processed_groupClause, root->processed_tlist);
}

/* The outputs as a target: what the store returns and the exact plan computes. */
static PathTarget*
output_target(PlannerInfo* root, List* outputs) {
    List* tlist = NIL;
    ListCell* lc;

    foreach (lc, outputs) {
        ChdbAggOutput* o = lfirst(lc);
        TargetEntry* tle =
            makeTargetEntry(o->expr, list_length(tlist) + 1, NULL, false);

        tle->ressortgroupref = o->sortgroupref;
        tlist                = lappend(tlist, tle);
    }
    return create_pathtarget(root, tlist);
}

/*
 * The Agg plan that answers when the store cannot: over the planner's
 * cheapest scan of the relation, grouping by hash as the hook requires of
 * the grouping, returning the outputs; the HAVING clauses stay with the
 * custom scan, which applies them to either answer.
 */
static Path*
exact_path(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* output_rel,
    PathTarget* target,
    double ngroups
) {
    AggClauseCosts costs = { 0 };

    get_agg_clause_costs(root, AGGSPLIT_SIMPLE, &costs);
    return (Path*)create_agg_path(
        root,
        output_rel,
        input_rel->cheapest_total_path,
        target,
        root->processed_groupClause ? AGG_HASHED : AGG_PLAIN,
        AGGSPLIT_SIMPLE,
        root->processed_groupClause,
        NIL,
        &costs,
        ngroups
    );
}

static void
add_aggregate_path(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* output_rel,
    IndexOptInfo* index,
    GroupPathExtraData* extra
) {
    ChdbPath* p  = palloc0(sizeof(*p));
    Path* path   = &p->cpath.path;
    List* having = (List*)extra->havingQual;
    List* exprs  = list_concat_copy(output_rel->reltarget->exprs, having);
    Relation rel_index, heap;
    ChdbColumn* cols;
    bool ok;
    double ngroups = 1;

    p->index         = index;
    p->spec.indexoid = index->indexoid;
    p->spec.limit    = -1;
    p->having        = having;
    chdb_planner_collect_quals(root, input_rel, p);
    if (p->local != NIL) {
        return;
    }
    rel_index = index_open(index->indexoid, NoLock);
    cols      = chdb_search_columns(rel_index);
    index_close(rel_index, NoLock);
    if (!chdb_planner_match_aggregates(
            root, input_rel, index, cols, exprs, &p->spec.agg_outputs
        )) {
        return;
    }
    heap = table_open(planner_rt_fetch(input_rel->relid, root)->relid, NoLock);
    ok   = chdb_planner_heap_all_visible(heap);
    table_close(heap, NoLock);
    if (!ok) {
        return;
    }
    if (root->processed_groupClause) {
        ngroups =
            estimate_num_groups(root, group_exprs(root), input_rel->rows, NULL, NULL);
    }

    path->type             = T_CustomPath;
    path->pathtype         = T_CustomScan;
    path->parent           = output_rel;
    path->pathtarget       = output_rel->reltarget;
    path->param_info       = NULL;
    path->parallel_aware   = false;
    path->parallel_safe    = false;
    path->parallel_workers = 0;
    path->pathkeys         = NIL;
    p->cpath.flags         = CUSTOMPATH_SUPPORT_PROJECTION;
    p->cpath.custom_paths  = list_make1(exact_path(
        root, input_rel, output_rel, output_target(root, p->spec.agg_outputs), ngroups
    ));
    p->cpath.methods       = &chdb_planner_agg_path_methods;
    chdb_planner_cost_aggregate(root, p, ngroups);
    add_path(output_rel, path);
}

static void
upper_paths_hook(
    PlannerInfo* root,
    UpperRelationKind stage,
    RelOptInfo* input_rel,
    RelOptInfo* output_rel,
    void* extra
) {
    GroupPathExtraData* gextra = extra;
    Query* parse               = root->parse;
    ListCell* lc;

    if (prev_hook) {
        prev_hook(root, stage, input_rel, output_rel, extra);
    }
    /*
     * The whole query is one relation's grouping: no grouping sets, which
     * the store does not do, and a grouping Postgres can hash, as the
     * exact plan does.
     */
    if (stage != UPPERREL_GROUP_AGG || !chdb_search_enable_custom_scan ||
        !chdb_search_enable_aggregate_pushdown ||
        gextra->patype != PARTITIONWISE_AGGREGATE_NONE || parse->groupingSets ||
        (parse->groupClause && !(gextra->flags & GROUPING_CAN_USE_HASH)) ||
        input_rel->reloptkind != RELOPT_BASEREL ||
        !chdb_planner_eligible_rel(
            input_rel, planner_rt_fetch(input_rel->relid, root)
        )) {
        return;
    }
    foreach (lc, input_rel->indexlist) {
        if (chdb_planner_usable_index(lfirst(lc))) {
            add_aggregate_path(root, input_rel, output_rel, lfirst(lc), gextra);
        }
    }
}

void
chdb_planner_aggregate_init(void) {
    DefineCustomBoolVariable(
        "chdb_search.enable_aggregate_pushdown",
        "Plan an aggregate over a chdb index as one statement the store computes.",
        "Off leaves the aggregate to Postgres over a scan. Needs "
        "chdb_search.enable_custom_scan.",
        &chdb_search_enable_aggregate_pushdown,
        true,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    prev_hook               = create_upper_paths_hook;
    create_upper_paths_hook = upper_paths_hook;
}
