/*
 * From the path to the plan and back: PlanCustomPath packs the spec into the
 * CustomScan, whose private data the plan cache copies and the executor
 * reads with chdb_planner_unpack. The clauses go in custom_exprs, where
 * setrefs.c adjusts their Vars like the scan's own quals; the numbers that
 * describe each one go in custom_private as Integer nodes, in the order
 * the unpacker expects: the index, the limit, then the quals and the
 * order-bys, each with a count first.
 */

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/restrictinfo.h"

#include "planner.h"

static List*
pack_int(List* list, int64 value) {
    return lappend(list, makeInteger((int)value));
}

static List*
pack_pushed(List* list, List* pushed) {
    ListCell* lc;

    list = pack_int(list, list_length(pushed));
    foreach (lc, pushed) {
        ChdbPushed* p = lfirst(lc);

        list = pack_int(list, p->attno);
        list = pack_int(list, p->strategy);
        list = pack_int(list, (int)p->subtype);
        list = pack_int(list, (int)p->collation);
        list = pack_int(list, p->argno);
    }
    return list;
}

static List*
pushed_clauses(List* pushed) {
    List* clauses = NIL;
    ListCell* lc;

    foreach (lc, pushed) {
        clauses = lappend(clauses, ((ChdbPushed*)lfirst(lc))->clause);
    }
    return clauses;
}

/* Whether a restriction is among the ones the store applies. */
static bool
is_pushed(const ChdbPath* p, RestrictInfo* rinfo) {
    ListCell* lc;

    foreach (lc, p->spec.quals) {
        if (((ChdbPushed*)lfirst(lc))->rinfo == rinfo) {
            return true;
        }
    }
    return false;
}

static Plan*
plan_custom_path(
    PlannerInfo* root,
    RelOptInfo* rel,
    CustomPath* best_path,
    List* tlist,
    List* clauses,
    List* custom_plans
) {
    ChdbPath* p       = (ChdbPath*)best_path;
    CustomScan* cscan = makeNode(CustomScan);
    List* local       = NIL;
    ListCell* lc;

    /* `clauses` are the relation's restrictions in execution order. */
    foreach (lc, clauses) {
        if (!is_pushed(p, lfirst(lc))) {
            local = lappend(local, lfirst(lc));
        }
    }
    cscan->scan.plan.targetlist = tlist;
    cscan->scan.plan.qual       = extract_actual_clauses(local, false);
    cscan->scan.scanrelid       = rel->relid;
    cscan->flags                = best_path->flags;
    cscan->custom_plans         = custom_plans;
    cscan->custom_exprs =
        list_concat(pushed_clauses(p->spec.quals), pushed_clauses(p->spec.orderbys));
    cscan->custom_private    = pack_int(NIL, (int)p->spec.indexoid);
    cscan->custom_private    = pack_int(cscan->custom_private, p->spec.limit);
    cscan->custom_private    = pack_pushed(cscan->custom_private, p->spec.quals);
    cscan->custom_private    = pack_pushed(cscan->custom_private, p->spec.orderbys);
    cscan->custom_scan_tlist = NIL;
    cscan->methods           = &chdb_planner_scan_methods;

    /* A change to the index, a REINDEX say, replans a cached statement. */
    root->glob->relationOids = lappend_oid(root->glob->relationOids, p->spec.indexoid);
    return &cscan->scan.plan;
}

const CustomPathMethods chdb_planner_path_methods = {
    .CustomName                      = "chdb_search",
    .PlanCustomPath                  = plan_custom_path,
    .ReparameterizeCustomPathByChild = NULL,
};

/* Reads the next Integer of custom_private. */
static int
unpack_int(ListCell** lc, List* list) {
    int value;

    if (*lc == NULL) {
        elog(ERROR, "chdb custom scan private data ends early");
    }
    value = intVal(lfirst(*lc));

    *lc = lnext(list, *lc);
    return value;
}

static List*
unpack_pushed(ListCell** lc, List* list, ListCell** clause, List* clauses) {
    List* pushed = NIL;
    int n        = unpack_int(lc, list);

    for (int i = 0; i < n; i++) {
        ChdbPushed* p = palloc0(sizeof(*p));

        p->attno     = unpack_int(lc, list);
        p->strategy  = unpack_int(lc, list);
        p->subtype   = (Oid)unpack_int(lc, list);
        p->collation = (Oid)unpack_int(lc, list);
        p->argno     = unpack_int(lc, list);
        p->clause    = lfirst(*clause);
        *clause      = lnext(clauses, *clause);
        pushed       = lappend(pushed, p);
    }
    return pushed;
}

ChdbScanSpec*
chdb_planner_unpack(const CustomScan* cscan) {
    ChdbScanSpec* spec = palloc0(sizeof(*spec));
    ListCell* lc       = list_head(cscan->custom_private);
    ListCell* clause   = list_head(cscan->custom_exprs);

    spec->indexoid = (Oid)unpack_int(&lc, cscan->custom_private);
    spec->limit    = unpack_int(&lc, cscan->custom_private);
    spec->quals =
        unpack_pushed(&lc, cscan->custom_private, &clause, cscan->custom_exprs);
    spec->orderbys =
        unpack_pushed(&lc, cscan->custom_private, &clause, cscan->custom_exprs);
    return spec;
}
