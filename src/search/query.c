/*
 * ScanKeys and query trees to ClickHouse SQL. Anything spliced into the
 * statement is either a function name picked from a fixed table
 * (textsearch.c), a column name quoted by columns.c, or a literal rendered
 * by literal.c with ClickHouse's own escaping rules (backslash and quote),
 * so a search string cannot break out.
 */

#include "postgres.h"

#include <string.h>

#include "utils/builtins.h"

#include "query.h"
#include "search.h"
#include "vector.h"

/* The column a leaf searches: the one it names, else the operator's. */
static const ChdbColumn*
leaf_column(const ChdbColumn* cols, int natts, int attno, const ChdbQuery* q) {
    if (!q->column) {
        if (attno < 1 || attno > natts) {
            elog(ERROR, "chdb query leaf names no column");
        }
        return &cols[attno - 1];
    }

    char* name = chdb_search_quote_ident(q->column);

    for (int i = 0; i < natts; i++) {
        if (strcmp(cols[i].name, name) == 0) {
            if (cols[i].kind != CHDB_COL_TEXT && cols[i].kind != CHDB_COL_TEXT_ARRAY) {
                ereport(
                    ERROR,
                    errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("chdb index column \"%s\" is not a text column", q->column)
                );
            }
            return &cols[i];
        }
    }
    ereport(
        ERROR,
        errcode(ERRCODE_UNDEFINED_COLUMN),
        errmsg("chdb index has no column named \"%s\"", q->column)
    );
}

/*
 * Groups are parenthesized and a pattern leaf is too (above), so a NOT
 * binds to its operand whatever it is, and a tree beside the other ANDed
 * keys stays one term of the WHERE.
 */
static void
render(
    StringInfo buf,
    const ChdbColumn* cols,
    int natts,
    int attno,
    const ChdbQuery* q
) {
    switch (q->kind) {
    case CHDB_Q_AND:
    case CHDB_Q_OR:
        appendStringInfoChar(buf, '(');
        for (int i = 0; i < q->nchildren; i++) {
            if (i) {
                appendStringInfoString(buf, q->kind == CHDB_Q_AND ? " AND " : " OR ");
            }
            render(buf, cols, natts, attno, q->children[i]);
        }
        appendStringInfoChar(buf, ')');
        return;
    case CHDB_Q_NOT:
        appendStringInfoString(buf, "NOT ");
        render(buf, cols, natts, attno, q->children[0]);
        return;
    case CHDB_Q_BOOST:
        render(buf, cols, natts, attno, q->children[0]);
        return;
    default:
        chdb_search_append_text_search(
            buf, leaf_column(cols, natts, attno, q), q->kind, q->needle, q->slop
        );
    }
}

static bool
has_not(const ChdbQuery* q) {
    if (q->kind == CHDB_Q_NOT) {
        return true;
    }
    for (int i = 0; i < q->nchildren; i++) {
        if (has_not(q->children[i])) {
            return true;
        }
    }
    return false;
}

/*
 * The operator is strict, so a NULL column never matches, while ClickHouse's
 * token functions answer false for a NULL text and a NOT would turn that
 * into a match: a tree with a NOT over a text column is guarded. An array
 * column needs none, as a NULL array is stored empty and the Postgres side
 * (queryeval.c) reads it as empty too.
 */
void
chdb_search_render_query(
    StringInfo buf,
    const ChdbColumn* cols,
    int natts,
    int attno,
    const ChdbQuery* q
) {
    bool guard = attno > 0 && cols[attno - 1].kind == CHDB_COL_TEXT && has_not(q);

    if (guard) {
        appendStringInfo(buf, "(%s IS NOT NULL AND ", cols[attno - 1].name);
    }
    render(buf, cols, natts, attno, q);
    if (guard) {
        appendStringInfoChar(buf, ')');
    }
}

static const char*
compare_operator(StrategyNumber strategy) {
    switch (strategy) {
    case CHDB_STRATEGY_EQ:
        return "=";
    case CHDB_STRATEGY_LT:
        return "<";
    case CHDB_STRATEGY_LE:
        return "<=";
    case CHDB_STRATEGY_GT:
        return ">";
    case CHDB_STRATEGY_GE:
        return ">=";
    }
    elog(ERROR, "unknown chdb comparison strategy %d", strategy);
}

bool
chdb_search_append_quals(
    StringInfo buf,
    const ChdbColumn* cols,
    int natts,
    ScanKey keys,
    int nkeys
) {
    for (int i = 0; i < nkeys; i++) {
        ScanKey key = &keys[i];
        const ChdbColumn* col;

        if (key->sk_flags & SK_ISNULL) {
            return false;
        }
        if (key->sk_attno < 1 || key->sk_attno > natts) {
            elog(ERROR, "chdb scan key on invalid column %d", key->sk_attno);
        }
        col = &cols[key->sk_attno - 1];
        if (i) {
            appendStringInfoString(buf, " AND ");
        }

        Oid argtype = OidIsValid(key->sk_subtype) ? key->sk_subtype : col->typid;

        if (key->sk_strategy >= CHDB_STRATEGY_EQ) {
            const char* op = compare_operator(key->sk_strategy);

            /* NaN and the infinities compare as Postgres does, not ClickHouse. */
            if (!chdb_search_append_special(
                    buf, col->name, key->sk_strategy, key->sk_argument, argtype
                )) {
                appendStringInfo(buf, "%s %s ", col->name, op);
                chdb_search_append_literal(buf, key->sk_argument, argtype);
            }
        } else if (key->sk_strategy == CHDB_STRATEGY_QUERY) {
            chdb_search_render_query(
                buf,
                cols,
                natts,
                key->sk_attno,
                chdb_search_query_from_datum(key->sk_argument)
            );
        } else {
            /* The needle is text: the operators of the text strategies say so. */
            chdb_search_append_text_search(
                buf, col, key->sk_strategy, TextDatumGetCString(key->sk_argument), 0
            );
        }
    }
    return true;
}

/* For the classes of other extensions, numbered as pgvector numbers them. */
static const char*
distance_function(StrategyNumber strategy) {
    switch (strategy) {
    case CHDB_ORDER_L2:
        return "L2Distance";
    case CHDB_ORDER_NEG_INNER_PRODUCT:
        return "-dotProduct";
    case CHDB_ORDER_COSINE:
        return "cosineDistance";
    case CHDB_ORDER_L1:
        return "L1Distance";
    }
    elog(ERROR, "unknown chdb order-by strategy %d", strategy);
}

char*
chdb_search_order_expr(Relation index, const ChdbColumn* cols, ScanKey orderby) {
    const ChdbColumn* col = &cols[orderby->sk_attno - 1];
    StringInfoData buf;
    Oid argtype = OidIsValid(orderby->sk_subtype) ? orderby->sk_subtype : col->typid;

    /* The operator is strict: every row gets a NULL distance, none is hidden. */
    if (orderby->sk_flags & SK_ISNULL) {
        return pstrdup("CAST(NULL AS Nullable(Float64))");
    }
    if (col->kind == CHDB_COL_VECTOR) {
        return chdb_search_vector_distance(index, col, orderby);
    }
    initStringInfo(&buf);
    appendStringInfo(
        &buf, "%s(%s, ", distance_function(orderby->sk_strategy), col->name
    );
    chdb_search_append_vector(&buf, orderby->sk_argument, argtype);
    appendStringInfoChar(&buf, ')');
    return buf.data;
}
