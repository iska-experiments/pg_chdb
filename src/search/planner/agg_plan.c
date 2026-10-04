/*
 * The aggregate scan's plan: a CustomScan of no relation, scanrelid 0,
 * whose custom_scan_tlist is the outputs, one entry per grouping column
 * and aggregate, so that setrefs.c rewrites the target list and the HAVING
 * clauses over them to references into the scan tuple (INDEX_VAR), as it
 * does for a foreign scan that pushed an aggregate down. setrefs.c rewrites
 * the pushed clauses in custom_exprs the same way, so the columns they
 * name follow the outputs as junk entries of the scan tuple, which the
 * store leaves NULL: a clause is evaluated for its argument only, and
 * EXPLAIN reads the column's name through the entry. The pushed clauses
 * and the spec's numbers go where plan.c puts them, and the outputs'
 * kinds, columns and types follow in custom_private; their expressions
 * are the custom_scan_tlist's. The Agg plan for the fallback is the one
 * child.
 */

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"

#include "planner.h"

static List*
add_entry(List* tlist, Expr* expr, bool junk) {
    return lappend(tlist, makeTargetEntry(expr, list_length(tlist) + 1, NULL, junk));
}

/* Whether an entry of `tlist` is `expr`. */
static bool
has_entry(List* tlist, Expr* expr) {
    ListCell* lc;

    foreach (lc, tlist) {
        if (equal(((TargetEntry*)lfirst(lc))->expr, expr)) {
            return true;
        }
    }
    return false;
}

static Plan*
plan_aggregate_path(
    PlannerInfo* root,
    RelOptInfo* rel,
    CustomPath* best_path,
    List* tlist,
    List* clauses,
    List* custom_plans
) {
    ChdbPath* p       = (ChdbPath*)best_path;
    CustomScan* cscan = chdb_planner_make_scan(root, p, tlist, p->having, custom_plans);
    ListCell* lc;

    cscan->scan.scanrelid = 0;
    cscan->methods        = &chdb_planner_agg_scan_methods;
    cscan->custom_private =
        chdb_planner_pack_int(cscan->custom_private, list_length(p->spec.agg_outputs));
    foreach (lc, p->spec.agg_outputs) {
        ChdbAggOutput* o = lfirst(lc);

        cscan->custom_scan_tlist = add_entry(cscan->custom_scan_tlist, o->expr, false);
        cscan->custom_private = chdb_planner_pack_int(cscan->custom_private, o->kind);
        cscan->custom_private = chdb_planner_pack_int(cscan->custom_private, o->attno);
        cscan->custom_private =
            chdb_planner_pack_int(cscan->custom_private, (int)o->typid);
    }
    foreach (lc, pull_var_clause((Node*)cscan->custom_exprs, 0)) {
        if (!has_entry(cscan->custom_scan_tlist, lfirst(lc))) {
            cscan->custom_scan_tlist =
                add_entry(cscan->custom_scan_tlist, lfirst(lc), true);
        }
    }
    return &cscan->scan.plan;
}

const CustomPathMethods chdb_planner_agg_path_methods = {
    .CustomName                      = "chdb_search aggregate",
    .PlanCustomPath                  = plan_aggregate_path,
    .ReparameterizeCustomPathByChild = NULL,
};

ChdbScanSpec*
chdb_planner_agg_unpack(const CustomScan* cscan) {
    ListCell* lc;
    ChdbScanSpec* spec = chdb_planner_unpack_at(cscan, &lc);
    int n              = chdb_planner_unpack_int(&lc, cscan->custom_private);

    for (int i = 0; i < n; i++) {
        ChdbAggOutput* o = palloc0(sizeof(*o));

        o->kind           = chdb_planner_unpack_int(&lc, cscan->custom_private);
        o->attno          = chdb_planner_unpack_int(&lc, cscan->custom_private);
        o->typid          = (Oid)chdb_planner_unpack_int(&lc, cscan->custom_private);
        o->expr           = ((TargetEntry*)list_nth(cscan->custom_scan_tlist, i))->expr;
        spec->agg_outputs = lappend(spec->agg_outputs, o);
    }
    return spec;
}
