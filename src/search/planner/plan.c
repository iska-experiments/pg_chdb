/*
 * From the path to the plan and back: PlanCustomPath packs the spec into the
 * CustomScan, whose private data the plan cache copies and the executor
 * reads with chdb_planner_unpack. The clauses go in custom_exprs, where
 * setrefs.c adjusts their Vars like the scan's own quals; the numbers that
 * describe each one go in custom_private as Integer nodes, in the order
 * the unpacker expects: the index, the limit, then the quals and the
 * order-bys, each with a count first, then the outputs' columns and the
 * one ordered by. A second stage packs the same prefix and its own data
 * after it, and reads on from where the unpacker leaves its cursor.
 *
 * With outputs the scan tuple is no longer the heap tuple: a
 * custom_scan_tlist describes it as the heap columns the query needs (its
 * target list, the quals kept here and the pushed clauses, for the recheck)
 * followed by the outputs' expressions, from which setrefs.c rewrites the
 * target list, the quals and the pushed clauses to refer to the scan tuple
 * by position, the chdb.score() calls among them, and the executor types
 * its virtual slot. The outputs are read back from its tail.
 */

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"

#include "planner.h"

List*
chdb_planner_pack_int(List* list, int64 value) {
    return lappend(list, makeInteger((int)value));
}

static List*
pack_pushed(List* list, List* pushed) {
    ListCell* lc;

    list = chdb_planner_pack_int(list, list_length(pushed));
    foreach (lc, pushed) {
        ChdbPushed* p = lfirst(lc);

        list = chdb_planner_pack_int(list, p->attno);
        list = chdb_planner_pack_int(list, p->strategy);
        list = chdb_planner_pack_int(list, (int)p->subtype);
        list = chdb_planner_pack_int(list, (int)p->collation);
        list = chdb_planner_pack_int(list, p->argno);
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

/*
 * The scan tuple with outputs: the relation's columns the plan refers to,
 * from the targets the planner gave the relation (its own, or the query's
 * final one when the relation is the whole query), the clauses kept here
 * and the pushed ones, each once; then the outputs.
 */
static List*
scan_tlist(RelOptInfo* rel, const ChdbPath* p, List* local) {
    List* exprs =
        list_concat_copy(rel->reltarget->exprs, p->cpath.path.pathtarget->exprs);
    List* tlist = NIL;
    ListCell* lc;

    exprs = list_concat(exprs, local);
    exprs = list_concat(exprs, pushed_clauses(p->spec.quals));
    exprs = list_concat(exprs, pushed_clauses(p->spec.orderbys));
    foreach (
        lc,
        pull_var_clause(
            (Node*)exprs,
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS
        )
    ) {
        if (!tlist_member(lfirst(lc), tlist)) {
            tlist = lappend(
                tlist, makeTargetEntry(lfirst(lc), list_length(tlist) + 1, NULL, false)
            );
        }
    }
    foreach (lc, p->spec.outputs) {
        tlist = lappend(
            tlist,
            makeTargetEntry(
                ((ChdbOutput*)lfirst(lc))->expr, list_length(tlist) + 1, "score", false
            )
        );
    }
    return tlist;
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

CustomScan*
chdb_planner_make_scan(
    PlannerInfo* root,
    ChdbPath* p,
    List* tlist,
    List* qual,
    List* custom_plans
) {
    CustomScan* cscan = makeNode(CustomScan);
    ListCell* lc;

    cscan->scan.plan.targetlist = tlist;
    cscan->scan.plan.qual       = qual;
    cscan->scan.scanrelid       = p->cpath.path.parent->relid;
    cscan->flags                = p->cpath.flags;
    cscan->custom_plans         = custom_plans;
    cscan->custom_exprs =
        list_concat(pushed_clauses(p->spec.quals), pushed_clauses(p->spec.orderbys));
    cscan->custom_private = chdb_planner_pack_int(NIL, (int)p->spec.indexoid);
    cscan->custom_private = chdb_planner_pack_int(cscan->custom_private, p->spec.limit);
    cscan->custom_private = pack_pushed(cscan->custom_private, p->spec.quals);
    cscan->custom_private = pack_pushed(cscan->custom_private, p->spec.orderbys);
    cscan->custom_private =
        chdb_planner_pack_int(cscan->custom_private, list_length(p->spec.outputs));
    foreach (lc, p->spec.outputs) {
        cscan->custom_private = chdb_planner_pack_int(
            cscan->custom_private, ((ChdbOutput*)lfirst(lc))->attno
        );
    }
    cscan->custom_private =
        chdb_planner_pack_int(cscan->custom_private, p->spec.score_order);
    cscan->custom_scan_tlist = NIL;
    cscan->methods           = &chdb_planner_scan_methods;

    /* A change to the index, a REINDEX say, replans a cached statement. */
    root->glob->relationOids = lappend_oid(root->glob->relationOids, p->spec.indexoid);
    return cscan;
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
    ChdbPath* p = (ChdbPath*)best_path;
    List* local = NIL;
    CustomScan* cscan;
    ListCell* lc;

    /* `clauses` are the relation's restrictions in execution order. */
    foreach (lc, clauses) {
        if (!is_pushed(p, lfirst(lc))) {
            local = lappend(local, lfirst(lc));
        }
    }
    cscan = chdb_planner_make_scan(
        root, p, tlist, extract_actual_clauses(local, false), custom_plans
    );
    if (p->spec.outputs) {
        cscan->custom_scan_tlist = scan_tlist(rel, p, cscan->scan.plan.qual);
        /*
         * A join above finds a call in the scan's own target list, where the
         * planner put only the columns; the whole query's target list, when
         * the relation is the query, replaces this one and needs no help.
         */
        for (lc = list_head(p->spec.outputs); lc && tlist;
             lc = lnext(p->spec.outputs, lc)) {
            tlist = lappend(
                tlist,
                makeTargetEntry(
                    ((ChdbOutput*)lfirst(lc))->expr, list_length(tlist) + 1, NULL, true
                )
            );
        }
        cscan->scan.plan.targetlist = tlist;
    }
    return &cscan->scan.plan;
}

const CustomPathMethods chdb_planner_path_methods = {
    .CustomName                      = "chdb_search",
    .PlanCustomPath                  = plan_custom_path,
    .ReparameterizeCustomPathByChild = NULL,
};

int
chdb_planner_unpack_int(ListCell** lc, List* list) {
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
    int n        = chdb_planner_unpack_int(lc, list);

    for (int i = 0; i < n; i++) {
        ChdbPushed* p = palloc0(sizeof(*p));

        p->attno     = chdb_planner_unpack_int(lc, list);
        p->strategy  = chdb_planner_unpack_int(lc, list);
        p->subtype   = (Oid)chdb_planner_unpack_int(lc, list);
        p->collation = (Oid)chdb_planner_unpack_int(lc, list);
        p->argno     = chdb_planner_unpack_int(lc, list);
        p->clause    = lfirst(*clause);
        *clause      = lnext(clauses, *clause);
        pushed       = lappend(pushed, p);
    }
    return pushed;
}

ChdbScanSpec*
chdb_planner_unpack_at(const CustomScan* cscan, ListCell** lc) {
    ChdbScanSpec* spec = palloc0(sizeof(*spec));
    ListCell* clause   = list_head(cscan->custom_exprs);
    int n;

    *lc            = list_head(cscan->custom_private);
    spec->indexoid = (Oid)chdb_planner_unpack_int(lc, cscan->custom_private);
    spec->limit    = chdb_planner_unpack_int(lc, cscan->custom_private);
    spec->quals =
        unpack_pushed(lc, cscan->custom_private, &clause, cscan->custom_exprs);
    spec->orderbys =
        unpack_pushed(lc, cscan->custom_private, &clause, cscan->custom_exprs);
    n = chdb_planner_unpack_int(lc, cscan->custom_private);
    for (int i = 0; i < n; i++) {
        ChdbOutput* out  = palloc0(sizeof(*out));
        TargetEntry* tle = list_nth(
            cscan->custom_scan_tlist, list_length(cscan->custom_scan_tlist) - n + i
        );

        out->attno    = chdb_planner_unpack_int(lc, cscan->custom_private);
        out->expr     = tle->expr;
        spec->outputs = lappend(spec->outputs, out);
    }
    spec->score_order = chdb_planner_unpack_int(lc, cscan->custom_private);
    return spec;
}

ChdbScanSpec*
chdb_planner_unpack(const CustomScan* cscan) {
    ListCell* lc;

    return chdb_planner_unpack_at(cscan, &lc);
}
