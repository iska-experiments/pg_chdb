/*
 * ScanKeys to ClickHouse SQL. Anything spliced into the statement is either
 * a function name picked from a fixed table, a column name quoted by ddl.c,
 * or a literal rendered by literal.c with ClickHouse's
 * own escaping rules (backslash and quote), so a search string cannot break
 * out.
 */

#include "postgres.h"

#include "query.h"
#include "search.h"

/*
 * ClickHouse's hasToken takes no array. The array tokenizer makes the needle
 * one token, so for arrays all, any and token agree and hasAllTokens serves;
 * a phrase of elements has no meaning, and ClickHouse would reject it.
 */
static const char*
text_function(StrategyNumber strategy, ChdbColumnKind kind) {
    if (kind == CHDB_COL_TEXT_ARRAY) {
        if (strategy == CHDB_STRATEGY_HAS_TOKEN) {
            return "hasAllTokens";
        }
        if (strategy == CHDB_STRATEGY_HAS_PHRASE) {
            ereport(
                ERROR,
                errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("chdb indexes do not search text[] columns for phrases")
            );
        }
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
            appendStringInfo(
                buf, "%s(%s, ", text_function(key->sk_strategy, col->kind), col->name
            );
            chdb_search_append_literal(buf, key->sk_argument, argtype);
            appendStringInfoChar(buf, ')');
        }
    }
    return true;
}

char*
chdb_search_order_expr(const ChdbColumn* cols, ScanKey orderby) {
    const ChdbColumn* col = &cols[orderby->sk_attno - 1];
    const char* fn;
    StringInfoData buf;
    Oid argtype = OidIsValid(orderby->sk_subtype) ? orderby->sk_subtype : col->typid;

    switch (orderby->sk_strategy) {
    case CHDB_ORDER_L2:
        fn = "L2Distance";
        break;
    case CHDB_ORDER_NEG_INNER_PRODUCT:
        fn = "-dotProduct";
        break;
    case CHDB_ORDER_COSINE:
        fn = "cosineDistance";
        break;
    case CHDB_ORDER_L1:
        fn = "L1Distance";
        break;
    default:
        elog(ERROR, "unknown chdb order-by strategy %d", orderby->sk_strategy);
    }

    /* The operator is strict: every row gets a NULL distance, none is hidden. */
    if (orderby->sk_flags & SK_ISNULL) {
        return pstrdup("CAST(NULL AS Nullable(Float64))");
    }
    initStringInfo(&buf);
    appendStringInfo(&buf, "%s(%s, ", fn, col->name);
    chdb_search_append_vector(&buf, orderby->sk_argument, argtype);
    appendStringInfoChar(&buf, ')');
    return buf.data;
}

char*
chdb_search_build_select(
    Relation index,
    ScanKey keys,
    int nkeys,
    ScanKey orderbys,
    int norderbys,
    int64 limit
) {
    StringInfoData buf, where;
    int natts = index->rd_att->natts;
    /* Once per statement; VACUUM's unqualified SELECT needs none of them. */
    ChdbColumn* cols = nkeys || norderbys ? chdb_search_columns(index) : NULL;

    initStringInfo(&buf);
    initStringInfo(&where);
    if (!chdb_search_append_quals(&where, cols, natts, keys, nkeys)) {
        return NULL;
    }

    appendStringInfoString(&buf, "SELECT ctid");
    for (int i = 0; i < norderbys; i++) {
        if (norderbys == 1) {
            appendStringInfo(
                &buf, ", %s AS _distance", chdb_search_order_expr(cols, &orderbys[i])
            );
        } else {
            appendStringInfo(
                &buf,
                ", %s AS _distance%d",
                chdb_search_order_expr(cols, &orderbys[i]),
                i + 1
            );
        }
    }
    appendStringInfo(&buf, " FROM %s", chdb_search_table_name(index));
    if (where.len) {
        appendStringInfo(&buf, " WHERE %s", where.data);
    }
    if (norderbys) {
        appendStringInfoString(&buf, " ORDER BY ");
        for (int i = 0; i < norderbys; i++) {
            if (norderbys == 1) {
                appendStringInfoString(&buf, "_distance");
            } else {
                appendStringInfo(&buf, "%s_distance%d", i ? ", " : "", i + 1);
            }
        }
    }
    if (limit >= 0) {
        appendStringInfo(&buf, " LIMIT " INT64_FORMAT, limit);
    }
    return buf.data;
}
