/*
 * What of a query the store can answer: a restriction clause that is an
 * operator of an indexed column's operator family, or a call of such an
 * operator's function (chdb.has_all_tokens(body, 'x') is `body @@@ 'x'` to
 * the store, though the index access method cannot use it: the planner
 * matches index clauses by operator only), and an ORDER BY pathkey that is
 * a distance operator of the family, ascending. Nothing here names an
 * operator or a function: the families say what they hold, the strategy
 * numbers come from pg_amop, and query.c renders a key by its number alone.
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/stratnum.h"
#include "catalog/pg_amop.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/catcache.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "planner.h"

/* Through the no-op relabelings the planner wraps binary-coercible types in. */
static Node*
strip(Node* node) {
    while (node && IsA(node, RelabelType)) {
        node = (Node*)((RelabelType*)node)->arg;
    }
    return node;
}

/* The index column `node` is a plain Var of, 1-based, or 0. */
static AttrNumber
index_column(RelOptInfo* rel, IndexOptInfo* index, Node* node) {
    Var* var = (Var*)strip(node);

    if (!var || !IsA(var, Var) || var->varno != rel->relid || var->varlevelsup != 0) {
        return 0;
    }
    for (int i = 0; i < index->nkeycolumns; i++) {
        if (index->indexkeys[i] != 0 && index->indexkeys[i] == var->varattno) {
            return i + 1;
        }
    }
    return 0;
}

/*
 * Whether `arg` is a value for the whole scan: no column of the relation,
 * so it does not change from row to row, and nothing volatile, which the
 * store would evaluate once where Postgres evaluates it per row.
 */
static bool
constant_for_scan(PlannerInfo* root, RelOptInfo* rel, Node* arg) {
    return !bms_overlap(pull_varnos(root, arg), rel->relids) &&
           !contain_volatile_functions(arg);
}

/*
 * The search operator of `family` whose function is `funcid`, taking
 * `lefttype` on the left, or InvalidOid: this is how `chdb.has_token(col,
 * 'x')` is found to mean `col @@= 'x'`, whichever schema the function is in.
 */
static Oid
operator_of_function(Oid family, Oid funcid, Oid lefttype) {
    CatCList* list = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(family));
    Oid opno       = InvalidOid;

    for (int i = 0; i < list->n_members && !OidIsValid(opno); i++) {
        Form_pg_amop op = (Form_pg_amop)GETSTRUCT(&list->members[i]->tuple);

        if (op->amoppurpose == AMOP_SEARCH && op->amoplefttype == lefttype &&
            get_opcode(op->amopopr) == funcid) {
            opno = op->amopopr;
        }
    }
    ReleaseCatCacheList(list);
    return opno;
}

/*
 * As the planner matches an index clause: the index column's collation, if
 * it has one, must be the operator's, or the store would compare under
 * another ordering than the query asks for.
 */
static bool
collation_matches(IndexOptInfo* index, AttrNumber attno, Oid collation) {
    Oid indexcoll = index->indexcollations[attno - 1];

    return !OidIsValid(indexcoll) || indexcoll == collation;
}

/*
 * Fills *out for an operator `opno` of the family of column `attno`, whose
 * argument is `argno` of the clause, checking it is a member for the search
 * or for the ordering and taking the argument's type from the family.
 */
static bool
fill(
    IndexOptInfo* index,
    AttrNumber attno,
    Oid opno,
    Oid collation,
    bool ordering,
    int argno,
    Expr* clause,
    ChdbPushed* out
) {
    Oid family = index->opfamily[attno - 1];
    Oid lefttype, righttype;
    int strategy;

    if (ordering ? !OidIsValid(get_op_opfamily_sortfamily(opno, family))
                 : get_op_opfamily_strategy(opno, family) == 0) {
        return false;
    }
    if (!collation_matches(index, attno, collation)) {
        return false;
    }
    get_op_opfamily_properties(
        opno, family, ordering, &strategy, &lefttype, &righttype
    );
    out->attno     = attno;
    out->strategy  = strategy;
    out->subtype   = righttype;
    out->collation = collation;
    out->argno     = argno;
    out->clause    = clause;
    out->rinfo     = NULL;
    return true;
}

bool
chdb_planner_match_clause(
    PlannerInfo* root,
    RelOptInfo* rel,
    IndexOptInfo* index,
    RestrictInfo* rinfo,
    ChdbPushed* out
) {
    Expr* clause = rinfo->clause;
    List* args;
    Oid opno = InvalidOid, funcid = InvalidOid, collation;
    AttrNumber attno;
    int argno;

    if (rinfo->pseudoconstant) {
        return false;
    }
    if (IsA(clause, OpExpr)) {
        opno      = ((OpExpr*)clause)->opno;
        args      = ((OpExpr*)clause)->args;
        collation = ((OpExpr*)clause)->inputcollid;
    } else if (IsA(clause, FuncExpr)) {
        funcid    = ((FuncExpr*)clause)->funcid;
        args      = ((FuncExpr*)clause)->args;
        collation = ((FuncExpr*)clause)->inputcollid;
    } else {
        return false;
    }
    if (list_length(args) != 2) {
        return false;
    }
    if ((attno = index_column(rel, index, linitial(args)))) {
        argno = 1;
    } else if (OidIsValid(opno) && (attno = index_column(rel, index, lsecond(args)))) {
        /* `5 < price` is `price > 5` to the family. */
        argno = 0;
        opno  = get_commutator(opno);
        if (!OidIsValid(opno)) {
            return false;
        }
    } else {
        return false;
    }
    if (!constant_for_scan(root, rel, list_nth(args, argno))) {
        return false;
    }
    if (OidIsValid(funcid)) {
        opno = operator_of_function(
            index->opfamily[attno - 1], funcid, exprType(linitial(args))
        );
        if (!OidIsValid(opno)) {
            return false;
        }
    }
    if (!fill(index, attno, opno, collation, false, argno, clause, out)) {
        return false;
    }
    out->rinfo = rinfo;
    return true;
}

/* Whether the pathkey sorts ascending with NULLs last, as a distance does. */
static bool
ascending(PathKey* pathkey) {
#if PG_VERSION_NUM >= 180000
    return pathkey->pk_cmptype == COMPARE_LT && !pathkey->pk_nulls_first;
#else
    return pathkey->pk_strategy == BTLessStrategyNumber && !pathkey->pk_nulls_first;
#endif
}

bool
chdb_planner_match_pathkey(
    PlannerInfo* root,
    RelOptInfo* rel,
    IndexOptInfo* index,
    PathKey* pathkey,
    ChdbPushed* out
) {
    EquivalenceClass* ec = pathkey->pk_eclass;
    ListCell* lc;

    if (!ascending(pathkey) || ec->ec_has_volatile) {
        return false;
    }
    foreach (lc, ec->ec_members) {
        EquivalenceMember* em = lfirst(lc);
        OpExpr* op            = (OpExpr*)strip((Node*)em->em_expr);
        AttrNumber attno;

        if (!bms_equal(em->em_relids, rel->relids) || !IsA(op, OpExpr) ||
            list_length(op->args) != 2) {
            continue;
        }
        attno = index_column(rel, index, linitial(op->args));
        if (!attno || !constant_for_scan(root, rel, lsecond(op->args))) {
            continue;
        }
        if (!fill(index, attno, op->opno, op->inputcollid, true, 1, (Expr*)op, out)) {
            continue;
        }
        /* The sort must be the one the ordering operator promises. */
        if (get_op_opfamily_sortfamily(op->opno, index->opfamily[attno - 1]) ==
            pathkey->pk_opfamily) {
            return true;
        }
    }
    return false;
}

Expr*
chdb_pushed_arg(const ChdbPushed* p) {
    List* args = IsA(p->clause, OpExpr) ? ((OpExpr*)p->clause)->args
                                        : ((FuncExpr*)p->clause)->args;

    return list_nth(args, p->argno);
}
