/*
 * The functions of schema chdb that build a chdb.query: the leaves
 * (chdb.match, match_all, term, phrase, regex, wildcard), chdb.boost,
 * chdb.in_column, and the combinations, as the operators && || and ! and
 * as chdb.all_of, any_of and none_of. Each reads its arguments' trees,
 * builds the new one and serializes it again (querytree.c).
 */

#include "postgres.h"

#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "query.h"
#include "search.h"

#define LEAF_BUILDER(name, kind)                                                       \
    PG_FUNCTION_INFO_V1(name);                                                         \
    Datum name(PG_FUNCTION_ARGS) {                                                     \
        return chdb_search_query_to_datum(                                             \
            chdb_search_query_leaf(kind, text_to_cstring(PG_GETARG_TEXT_PP(0)), 0)     \
        );                                                                             \
    }

LEAF_BUILDER(chdb_search_q_match_any, CHDB_Q_MATCH_ANY)
LEAF_BUILDER(chdb_search_q_match_all, CHDB_Q_MATCH_ALL)
LEAF_BUILDER(chdb_search_q_term, CHDB_Q_TERM)
LEAF_BUILDER(chdb_search_q_regex, CHDB_Q_REGEX)
LEAF_BUILDER(chdb_search_q_wildcard, CHDB_Q_WILDCARD)

PG_FUNCTION_INFO_V1(chdb_search_q_phrase);
Datum
chdb_search_q_phrase(PG_FUNCTION_ARGS) {
    return chdb_search_query_to_datum(chdb_search_query_leaf(
        CHDB_Q_PHRASE, text_to_cstring(PG_GETARG_TEXT_PP(0)), PG_GETARG_INT32(1)
    ));
}

PG_FUNCTION_INFO_V1(chdb_search_q_boost);
Datum
chdb_search_q_boost(PG_FUNCTION_ARGS) {
    return chdb_search_query_to_datum(chdb_search_query_boost(
        chdb_search_query_from_datum(PG_GETARG_DATUM(0)), PG_GETARG_FLOAT4(1)
    ));
}

PG_FUNCTION_INFO_V1(chdb_search_q_in_column);
Datum
chdb_search_q_in_column(PG_FUNCTION_ARGS) {
    ChdbQuery* q = chdb_search_query_from_datum(PG_GETARG_DATUM(0));

    chdb_search_query_in_column(q, text_to_cstring(PG_GETARG_TEXT_PP(1)));
    return chdb_search_query_to_datum(q);
}

/*
 * Combines queries under one and, or or not, splicing in the children of
 * an operand of the same kind, so that a && b && c is one group of three.
 */
static Datum
combine(ChdbQueryKind kind, int n, ChdbQuery** qs) {
    int count = 0;

    for (int i = 0; i < n; i++) {
        count += qs[i]->kind == kind ? qs[i]->nchildren : 1;
    }

    ChdbQuery** children = palloc(sizeof(ChdbQuery*) * Max(count, 1));
    int at               = 0;

    for (int i = 0; i < n; i++) {
        if (qs[i]->kind == kind) {
            for (int j = 0; j < qs[i]->nchildren; j++) {
                children[at++] = qs[i]->children[j];
            }
        } else {
            children[at++] = qs[i];
        }
    }
    return chdb_search_query_to_datum(chdb_search_query_group(kind, count, children));
}

static Datum
combine_args(PG_FUNCTION_ARGS, ChdbQueryKind kind) {
    int n          = PG_NARGS();
    ChdbQuery** qs = palloc(sizeof(ChdbQuery*) * n);

    for (int i = 0; i < n; i++) {
        qs[i] = chdb_search_query_from_datum(PG_GETARG_DATUM(i));
    }
    return combine(kind, n, qs);
}

/* The variadic forms: every element must be a query. */
static Datum
combine_array(PG_FUNCTION_ARGS, ChdbQueryKind kind) {
    ArrayType* arr = PG_GETARG_ARRAYTYPE_P(0);
    Datum* elems;
    bool* nulls;
    int n;

    deconstruct_array(
        arr, ARR_ELEMTYPE(arr), -1, false, TYPALIGN_INT, &elems, &nulls, &n
    );

    ChdbQuery** qs = palloc(sizeof(ChdbQuery*) * Max(n, 1));

    for (int i = 0; i < n; i++) {
        if (nulls[i]) {
            ereport(
                ERROR,
                errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                errmsg("a chdb.query combination cannot include a null query")
            );
        }
        qs[i] = chdb_search_query_from_datum(elems[i]);
    }
    return combine(kind, n, qs);
}

PG_FUNCTION_INFO_V1(chdb_search_q_and);
Datum
chdb_search_q_and(PG_FUNCTION_ARGS) {
    return combine_args(fcinfo, CHDB_Q_AND);
}

PG_FUNCTION_INFO_V1(chdb_search_q_or);
Datum
chdb_search_q_or(PG_FUNCTION_ARGS) {
    return combine_args(fcinfo, CHDB_Q_OR);
}

/* not(not(q)) stays as written: a double negation is the user's to see. */
PG_FUNCTION_INFO_V1(chdb_search_q_not);
Datum
chdb_search_q_not(PG_FUNCTION_ARGS) {
    ChdbQuery** child = palloc(sizeof(ChdbQuery*));

    child[0] = chdb_search_query_from_datum(PG_GETARG_DATUM(0));
    return chdb_search_query_to_datum(chdb_search_query_group(CHDB_Q_NOT, 1, child));
}

PG_FUNCTION_INFO_V1(chdb_search_q_all_of);
Datum
chdb_search_q_all_of(PG_FUNCTION_ARGS) {
    return combine_array(fcinfo, CHDB_Q_AND);
}

PG_FUNCTION_INFO_V1(chdb_search_q_any_of);
Datum
chdb_search_q_any_of(PG_FUNCTION_ARGS) {
    return combine_array(fcinfo, CHDB_Q_OR);
}
