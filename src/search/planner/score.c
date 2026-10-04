/*
 * Binding chdb.score() to the scan. The function is a placeholder (the
 * access method's score.c), so the planner must find every call the query
 * makes on the relation and have the store compute it: in the target list,
 * where the ORDER BY and the window clauses put their expressions too, in
 * HAVING, and in the relation's own restrictions, for a WHERE on the score.
 * A call binds through its first argument, a column of the relation, any
 * column; a second argument names the one indexed text column to score. A
 * DESC pathkey on a bound call is an order the store can serve.
 */

#include "postgres.h"

#include <string.h>

#include "access/genam.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include "../search.h"
#include "planner.h"

typedef struct Collect {
    PlannerInfo* root;
    RelOptInfo* rel;
    IndexOptInfo* index;
    Relation rel_index; /* opened on the first call with a column name */
    List* found;
} Collect;

/* The index column of `index` named `name`, if it is a text column; else 0. */
static AttrNumber
text_column(Collect* c, const char* name) {
    Oid relid = planner_rt_fetch(c->rel->relid, c->root)->relid;

    for (int i = 0; i < c->index->nkeycolumns; i++) {
        AttrNumber attno = c->index->indexkeys[i];
        char* attname    = attno ? get_attname(relid, attno, true) : NULL;
        ChdbColumnKind kind;

        if (!attname || strcmp(attname, name) != 0) {
            continue;
        }
        if (!c->rel_index) {
            c->rel_index = index_open(c->index->indexoid, NoLock);
        }
        kind = chdb_search_proc_kind(index_getprocid(c->rel_index, i + 1, 1));
        return kind == CHDB_COL_TEXT || kind == CHDB_COL_TEXT_ARRAY ? i + 1 : 0;
    }
    return 0;
}

/* Records a call on the relation, once per distinct expression. */
static void
bind(Collect* c, FuncExpr* f) {
    Var* var         = (Var*)chdb_planner_strip(linitial(f->args));
    AttrNumber attno = 0;
    ListCell* lc;

    if (!IsA(var, Var) || var->varno != c->rel->relid || var->varlevelsup != 0) {
        return;
    }
    if (list_length(f->args) == 2) {
        Const* name = (Const*)lsecond(f->args);

        if (!IsA(name, Const) || name->constisnull || name->consttype != TEXTOID) {
            return;
        }
        attno = text_column(c, TextDatumGetCString(name->constvalue));
        if (!attno) {
            return;
        }
    }
    foreach (lc, c->found) {
        if (equal(((ChdbOutput*)lfirst(lc))->expr, f)) {
            return;
        }
    }

    ChdbOutput* out = palloc0(sizeof(*out));

    out->expr  = (Expr*)f;
    out->attno = attno;
    c->found   = lappend(c->found, out);
}

/* Sub-queries bind their own calls when they are planned. */
static bool
walker(Node* node, Collect* c) {
    if (!node) {
        return false;
    }
    if (IsA(node, FuncExpr) && chdb_search_is_score(((FuncExpr*)node)->funcid)) {
        bind(c, (FuncExpr*)node);
    }
    return expression_tree_walker(node, walker, c);
}

List*
chdb_planner_collect_scores(PlannerInfo* root, RelOptInfo* rel, IndexOptInfo* index) {
    Collect c = { .root = root, .rel = rel, .index = index };
    ListCell* lc;

    /*
     * A row lock, an UPDATE or a DELETE may recheck a row under EvalPlanQual,
     * which hands the scan the heap tuple as the scan tuple; with outputs the
     * scan tuple is virtual, so no output then: the call raises instead.
     */
    if (root->parse->commandType != CMD_SELECT || root->rowMarks != NIL) {
        return NIL;
    }
    walker((Node*)root->parse->targetList, &c);
    walker(root->parse->havingQual, &c);
    foreach (lc, rel->baserestrictinfo) {
        walker((Node*)((RestrictInfo*)lfirst(lc))->clause, &c);
    }
    if (c.rel_index) {
        index_close(c.rel_index, NoLock);
    }
    return c.found;
}

/* Descending, as a score is ordered; NULLS either way, as a score has none. */
static bool
descending(PathKey* pathkey) {
#if PG_VERSION_NUM >= 180000
    return pathkey->pk_cmptype == COMPARE_GT;
#else
    return pathkey->pk_strategy == BTGreaterStrategyNumber;
#endif
}

bool
chdb_planner_match_score_pathkey(PathKey* pathkey, List* outputs, int* n) {
    EquivalenceClass* ec = pathkey->pk_eclass;
    ListCell *lm, *lo;

    if (outputs == NIL || !descending(pathkey) || ec->ec_has_volatile) {
        return false;
    }
    foreach (lm, ec->ec_members) {
        Node* expr =
            chdb_planner_strip((Node*)((EquivalenceMember*)lfirst(lm))->em_expr);
        int i = 1;

        foreach (lo, outputs) {
            if (equal(((ChdbOutput*)lfirst(lo))->expr, expr)) {
                *n = i;
                return true;
            }
            i++;
        }
    }
    return false;
}
