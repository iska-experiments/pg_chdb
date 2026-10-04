/*
 * ScanKeys to ClickHouse SQL. Anything spliced into the statement is either
 * a function name picked from a fixed table, a column name quoted by ddl.c,
 * or a literal rendered by literal.c with ClickHouse's
 * own escaping rules (backslash and quote), so a search string cannot break
 * out.
 */

#include "postgres.h"

#include "utils/builtins.h"

#include "query.h"
#include "search.h"
#include "vector.h"

/*
 * ClickHouse's hasToken takes no array. The array tokenizer makes the needle
 * one token, so for arrays all, any and token agree and hasAllTokens serves;
 * a phrase or a pattern over elements has no meaning, and ClickHouse would
 * reject it.
 */
static const char*
token_function(StrategyNumber strategy, ChdbColumnKind kind) {
    if (kind == CHDB_COL_TEXT_ARRAY && strategy == CHDB_STRATEGY_HAS_TOKEN) {
        return "hasAllTokens";
    }
    switch (strategy) {
    case CHDB_STRATEGY_HAS_ALL_TOKENS:
        return "hasAllTokens";
    case CHDB_STRATEGY_HAS_ANY_TOKENS:
        return "hasAnyTokens";
    case CHDB_STRATEGY_HAS_TOKEN:
        return "hasToken";
    case CHDB_STRATEGY_HAS_PHRASE:
        return "hasPhrase";
    }
    elog(ERROR, "unknown chdb text strategy %d", strategy);
}

/*
 * A text search of `col` by the strategy of its operator. The token
 * searches are the index's own functions, which preprocess the needle as
 * they do the column. A regular expression and a LIKE pattern read the
 * text, so they take the column through its preprocessor, with the pattern
 * folded alike: match() by RE2's (?i), LIKE through the same function. A
 * LIKE is parenthesized, as it may follow a NOT.
 */
void
chdb_search_append_text_search(
    StringInfo buf,
    const ChdbColumn* col,
    StrategyNumber strategy,
    const char* needle
) {
    bool array = col->kind == CHDB_COL_TEXT_ARRAY;
    StringInfoData lit;

    if (array && strategy >= CHDB_STRATEGY_HAS_PHRASE) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg(
                "chdb indexes do not search text[] columns for %s",
                strategy == CHDB_STRATEGY_HAS_PHRASE ? "phrases" : "patterns"
            )
        );
    }
    initStringInfo(&lit);
    if (strategy == CHDB_STRATEGY_REGEX && chdb_search_folds_case(col)) {
        needle = psprintf("(?i)%s", needle);
    }
    chdb_search_append_string(&lit, needle);
    switch (strategy) {
    case CHDB_STRATEGY_REGEX:
        appendStringInfo(
            buf,
            "match(%s, %s)",
            chdb_search_preprocessed(col, col->name, false),
            lit.data
        );
        return;
    case CHDB_STRATEGY_WILDCARD:
        appendStringInfo(
            buf,
            "(%s LIKE %s)",
            chdb_search_preprocessed(col, col->name, false),
            chdb_search_folds_case(col) ? chdb_search_preprocessed(col, lit.data, true)
                                        : lit.data
        );
        return;
    }
    appendStringInfo(
        buf, "%s(%s, %s)", token_function(strategy, col->kind), col->name, lit.data
    );
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
        } else {
            /* The needle is text: the operators of the text strategies say so. */
            chdb_search_append_text_search(
                buf, col, key->sk_strategy, TextDatumGetCString(key->sk_argument)
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
