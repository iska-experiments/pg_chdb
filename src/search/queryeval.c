/*
 * The Postgres implementation of `col @@@ query`: a sequential scan, or the
 * recheck of a plan that does not use the index, evaluates the tree with the
 * same per-leaf fallbacks as the operators of each strategy (ops.c,
 * pattern.c), so it answers as the index does for the default tokenizer and
 * preprocessor. A leaf that names a column of its own cannot be evaluated
 * here, where only the operator's column is at hand.
 *
 * The text form is strict. The array form reads a NULL array as empty, as
 * the store holds it (ClickHouse arrays are never NULL), so that a NOT over
 * an array column answers alike through the index and here.
 */

#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "query.h"
#include "search.h"

static bool
matches(const ChdbQuery* q, Datum hay, bool array, Oid collation) {
    switch (q->kind) {
    case CHDB_Q_AND:
        for (int i = 0; i < q->nchildren; i++) {
            if (!matches(q->children[i], hay, array, collation)) {
                return false;
            }
        }
        return true;
    case CHDB_Q_OR:
        for (int i = 0; i < q->nchildren; i++) {
            if (matches(q->children[i], hay, array, collation)) {
                return true;
            }
        }
        return false;
    case CHDB_Q_NOT:
        return !matches(q->children[0], hay, array, collation);
    case CHDB_Q_BOOST:
        return matches(q->children[0], hay, array, collation);
    default:
        break;
    }
    if (q->column) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg(
                "a chdb.query naming a column is evaluated through a chdb index only"
            ),
            errhint("Search the column it names directly, or let the index scan run.")
        );
    }

    text* needle = cstring_to_text(q->needle);

    return array ? chdb_search_array_leaf(q->kind, DatumGetArrayTypeP(hay), needle)
                 : chdb_search_text_leaf(
                       q->kind, DatumGetTextPP(hay), needle, q->slop, collation
                   );
}

PG_FUNCTION_INFO_V1(chdb_search_query_matches);
Datum
chdb_search_query_matches(PG_FUNCTION_ARGS) {
    ChdbQuery* q = chdb_search_query_from_datum(PG_GETARG_DATUM(1));

    PG_RETURN_BOOL(matches(q, PG_GETARG_DATUM(0), false, PG_GET_COLLATION()));
}

PG_FUNCTION_INFO_V1(chdb_search_array_query_matches);
Datum
chdb_search_array_query_matches(PG_FUNCTION_ARGS) {
    ChdbQuery* q;
    Datum arr;

    if (PG_ARGISNULL(1)) {
        PG_RETURN_NULL();
    }
    q   = chdb_search_query_from_datum(PG_GETARG_DATUM(1));
    arr = PG_ARGISNULL(0) ? PointerGetDatum(construct_empty_array(TEXTOID))
                          : PG_GETARG_DATUM(0);
    PG_RETURN_BOOL(matches(q, arr, true, PG_GET_COLLATION()));
}
