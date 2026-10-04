/*
 * What of a grouped target the store can compute. The target of a GROUP BY
 * or aggregate query is expressions over grouping columns and aggregates;
 * the store computes the leaves, one column each, and Postgres the rest.
 * A leaf is a Var, which must be a GROUP BY column of the index, or an
 * Aggref of one of count, min, max, sum and avg over an index column, in
 * the plain form: no DISTINCT, ORDER BY or FILTER. Unlike the operators of
 * match.c, which the operator families name, the aggregates are told by
 * their names in pg_catalog: there is no catalog structure that says what
 * sum means, and the store's sum must be Postgres's.
 *
 * Which columns serve what: count(col) and GROUP BY need the column stored
 * as written, which a text or plain column is and a text[] is not (a NULL
 * array is stored empty); min, max, sum and avg need a plain column, as
 * columnar_ops stores, whose comparisons are bytewise like ClickHouse's.
 */

#include "postgres.h"

#include <string.h>

#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/tlist.h"
#include "utils/lsyscache.h"

#include "../search.h"
#include "planner.h"

/* The aggregate's one argument as an index column of a kind that serves it. */
static AttrNumber
arg_column(
    RelOptInfo* rel,
    IndexOptInfo* index,
    const ChdbColumn* cols,
    Aggref* aggref,
    bool plain_only
) {
    TargetEntry* tle = linitial(aggref->args);
    AttrNumber attno = chdb_planner_index_column(rel, index, (Node*)tle->expr);

    if (!attno) {
        return 0;
    }
    switch (cols[attno - 1].kind) {
    case CHDB_COL_COLUMNAR:
        return attno;
    case CHDB_COL_TEXT:
        return plain_only ? 0 : attno;
    default:
        return 0;
    }
}

/* The type Postgres's sum or avg of `argtype` has, or InvalidOid. */
static Oid
sum_type(Oid argtype, bool avg) {
    switch (argtype) {
    case INT2OID:
    case INT4OID:
        return avg ? NUMERICOID : INT8OID;
    case INT8OID:
    case NUMERICOID:
        return NUMERICOID;
    case FLOAT4OID:
        return avg ? FLOAT8OID : FLOAT4OID;
    case FLOAT8OID:
        return FLOAT8OID;
    }
    return InvalidOid;
}

/*
 * Whether `aggref` is one the store computes, filling *out. The column's
 * collation, if it has one, must be the aggregate's, or the store's min
 * would not be Postgres's.
 */
static bool
match_aggref(
    RelOptInfo* rel,
    IndexOptInfo* index,
    const ChdbColumn* cols,
    Aggref* aggref,
    ChdbAggOutput* out
) {
    const char* name;
    AttrNumber attno;
    Oid argtype;

    if (aggref->aggkind != AGGKIND_NORMAL || aggref->aggsplit != AGGSPLIT_SIMPLE ||
        aggref->aggdistinct || aggref->aggorder || aggref->aggfilter ||
        aggref->aggvariadic ||
        get_func_namespace(aggref->aggfnoid) != PG_CATALOG_NAMESPACE) {
        return false;
    }
    name       = get_func_name(aggref->aggfnoid);
    out->expr  = (Expr*)aggref;
    out->typid = aggref->aggtype;
    if (aggref->aggstar) {
        out->kind  = CHDB_AGG_COUNT;
        out->attno = 0;
        return strcmp(name, "count") == 0 && aggref->aggtype == INT8OID;
    }
    if (list_length(aggref->args) != 1) {
        return false;
    }
    if (strcmp(name, "count") == 0) {
        out->kind  = CHDB_AGG_COUNT_COL;
        out->attno = arg_column(rel, index, cols, aggref, false);
        return out->attno != 0 && aggref->aggtype == INT8OID;
    }
    attno = arg_column(rel, index, cols, aggref, true);
    if (!attno || (OidIsValid(index->indexcollations[attno - 1]) &&
                   index->indexcollations[attno - 1] != aggref->inputcollid)) {
        return false;
    }
    out->attno = attno;
    argtype    = cols[attno - 1].typid;
    if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0) {
        out->kind = name[1] == 'i' ? CHDB_AGG_MIN : CHDB_AGG_MAX;
        return aggref->aggtype == argtype;
    }
    if (strcmp(name, "sum") == 0 || strcmp(name, "avg") == 0) {
        out->kind = name[0] == 's' ? CHDB_AGG_SUM : CHDB_AGG_AVG;
        return aggref->aggtype == sum_type(argtype, out->kind == CHDB_AGG_AVG);
    }
    return false;
}

/* Whether an equal expression is already an output. */
static bool
is_output(List* outputs, Expr* expr) {
    ListCell* lc;

    foreach (lc, outputs) {
        if (equal(((ChdbAggOutput*)lfirst(lc))->expr, expr)) {
            return true;
        }
    }
    return false;
}

/*
 * The GROUP BY columns as outputs, all of them, whether or not the target
 * names them: the store must group by the same. Each must be an index
 * column stored as written, under a collation whose equality is bytewise,
 * as the store groups bytes.
 */
static bool
group_outputs(
    PlannerInfo* root,
    RelOptInfo* rel,
    IndexOptInfo* index,
    const ChdbColumn* cols,
    List** outputs
) {
    ListCell* lc;

    foreach (lc, root->processed_groupClause) {
        SortGroupClause* sgc = lfirst(lc);
        Var* var = (Var*)get_sortgroupclause_expr(sgc, root->processed_tlist);
        ChdbAggOutput* out;
        AttrNumber attno;

        while (IsA(var, RelabelType)) {
            var = (Var*)((RelabelType*)var)->arg;
        }
        attno = chdb_planner_index_column(rel, index, (Node*)var);
        if (!attno || !IsA(var, Var) || cols[attno - 1].kind == CHDB_COL_TEXT_ARRAY ||
            cols[attno - 1].kind == CHDB_COL_VECTOR ||
            (OidIsValid(var->varcollid) &&
             !get_collation_isdeterministic(var->varcollid))) {
            return false;
        }
        if (is_output(*outputs, (Expr*)var)) {
            continue;
        }
        out               = palloc0(sizeof(*out));
        out->kind         = CHDB_AGG_GROUP;
        out->attno        = attno;
        out->typid        = var->vartype;
        out->expr         = (Expr*)var;
        out->sortgroupref = sgc->tleSortGroupRef;
        *outputs          = lappend(*outputs, out);
    }
    return true;
}

bool
chdb_planner_match_aggregates(
    PlannerInfo* root,
    RelOptInfo* rel,
    IndexOptInfo* index,
    const ChdbColumn* cols,
    List* exprs,
    List** outputs
) {
    List* leaves;
    ListCell* lc;

    *outputs = NIL;
    if (!group_outputs(root, rel, index, cols, outputs)) {
        return false;
    }
    leaves = pull_var_clause(
        (Node*)exprs,
        PVC_INCLUDE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS
    );
    foreach (lc, leaves) {
        Expr* leaf = lfirst(lc);
        ChdbAggOutput* out;

        if (is_output(*outputs, leaf)) {
            continue;
        }
        /* A Var not grouped by is a column the parser found dependent on a key. */
        if (!IsA(leaf, Aggref)) {
            return false;
        }
        out = palloc0(sizeof(*out));
        if (!match_aggref(rel, index, cols, (Aggref*)leaf, out)) {
            return false;
        }
        *outputs = lappend(*outputs, out);
    }
    return true;
}
