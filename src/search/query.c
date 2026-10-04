/*
 * ScanKeys to ClickHouse SQL. Anything spliced into the statement is either
 * a function name picked from a fixed table, a column name quoted by
 * pgch_quote_ch_ident, or a literal rendered here with ClickHouse's own
 * escaping rules (backslash and quote), so a search string cannot break out.
 */

#include "postgres.h"

#include <math.h>
#include <string.h>

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"

#include "query.h"
#include "search.h"

/* Quotes `s` as a ClickHouse string literal. */
void
chdb_search_append_string(StringInfo buf, const char* s) {
    appendStringInfoChar(buf, '\'');
    for (; *s; s++) {
        if (*s == '\\' || *s == '\'') {
            appendStringInfoChar(buf, '\\');
        }
        appendStringInfoChar(buf, *s);
    }
    appendStringInfoChar(buf, '\'');
}

static char*
output_of(Datum value, Oid typid) {
    Oid out;
    bool varlena;

    getTypeOutputInfo(typid, &out, &varlena);
    return OidOutputFunctionCall(out, value);
}

static void
unsupported(Oid typid) {
    ereport(
        ERROR,
        errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        errmsg(
            "cannot compare a chdb index column with a value of type %s",
            format_type_be(typid)
        )
    );
}

void
chdb_search_append_literal(StringInfo buf, Datum value, Oid typid) {
    switch (typid) {
    case TEXTOID:
    case VARCHAROID:
    case BPCHAROID:
    case NAMEOID:
        chdb_search_append_string(buf, output_of(value, typid));
        return;
    case INT2OID:
    case INT4OID:
    case INT8OID:
        appendStringInfoString(buf, output_of(value, typid));
        return;
    case FLOAT4OID:
    case FLOAT8OID: {
        char* s = output_of(value, typid);

        if (strcmp(s, "NaN") == 0) {
            s = "nan";
        } else if (strcmp(s, "Infinity") == 0) {
            s = "inf";
        } else if (strcmp(s, "-Infinity") == 0) {
            s = "-inf";
        }
        appendStringInfoString(buf, s);
        return;
    }
    case NUMERICOID: {
        char* s = output_of(value, typid);
        char* dot;

        if (strcmp(s, "NaN") == 0 || strstr(s, "Infinity")) {
            unsupported(typid);
        }
        dot = strchr(s, '.');
        /* A Decimal literal keeps its scale, which a bare 1.50 would lose. */
        appendStringInfo(
            buf, "toDecimal256('%s', %d)", s, dot ? (int)strlen(dot + 1) : 0
        );
        return;
    }
    case BOOLOID:
        appendStringInfoString(buf, DatumGetBool(value) ? "true" : "false");
        return;
    case UUIDOID:
        appendStringInfo(buf, "toUUID('%s')", output_of(value, typid));
        return;
    case DATEOID: {
        DateADT d = DatumGetDateADT(value);

        if (DATE_NOT_FINITE(d)) {
            unsupported(typid);
        }
        /* PostgreSQL counts days from 2000-01-01, ClickHouse from 1970. */
        appendStringInfo(
            buf, "toDate32(%d)", (int)(d + (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE))
        );
        return;
    }
    case TIMESTAMPOID:
    case TIMESTAMPTZOID: {
        Timestamp ts = DatumGetTimestamp(value);

        if (TIMESTAMP_NOT_FINITE(ts)) {
            unsupported(typid);
        }
        /* Columns are DateTime64(6, 'UTC'), and a timestamp without zone is read as
         * UTC. */
        appendStringInfo(
            buf,
            "fromUnixTimestamp64Micro(" INT64_FORMAT ", 'UTC')",
            (int64)ts + (int64)(POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * USECS_PER_DAY
        );
        return;
    }
    default:
        unsupported(typid);
    }
}

/*
 * A vector or real[] argument as a ClickHouse array literal. Both output as
 * `[1,2,3]` (vector) or `{1,2,3}` (array); only digits and number punctuation
 * are accepted, so nothing else can reach the statement.
 */
static void
append_vector(StringInfo buf, Datum value, Oid typid) {
    char* s = output_of(value, typid);

    for (char* p = s; *p; p++) {
        if (*p == '{') {
            *p = '[';
        } else if (*p == '}') {
            *p = ']';
        } else if (!strchr("0123456789.eE+-,[]", *p)) {
            ereport(
                ERROR,
                errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("unsupported value in vector argument: \"%s\"", s)
            );
        }
    }
    appendStringInfoString(buf, s);
}

static const char*
text_function(StrategyNumber strategy) {
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
chdb_search_append_quals(StringInfo buf, Relation index, ScanKey keys, int nkeys) {
    ChdbColumn* cols = chdb_search_columns(index);

    for (int i = 0; i < nkeys; i++) {
        ScanKey key = &keys[i];
        ChdbColumn* col;

        if (key->sk_flags & SK_ISNULL) {
            return false;
        }
        if (key->sk_attno < 1 || key->sk_attno > index->rd_att->natts) {
            elog(ERROR, "chdb scan key on invalid column %d", key->sk_attno);
        }
        col = &cols[key->sk_attno - 1];
        if (i) {
            appendStringInfoString(buf, " AND ");
        }

        Oid argtype = OidIsValid(key->sk_subtype) ? key->sk_subtype : col->typid;

        if (col->kind == CHDB_COL_COLUMNAR) {
            appendStringInfo(
                buf, "%s %s ", col->name, compare_operator(key->sk_strategy)
            );
            chdb_search_append_literal(buf, key->sk_argument, argtype);
        } else {
            appendStringInfo(
                buf, "%s(%s, ", text_function(key->sk_strategy), col->name
            );
            chdb_search_append_literal(buf, key->sk_argument, TEXTOID);
            appendStringInfoChar(buf, ')');
        }
    }
    return true;
}

char*
chdb_search_order_expr(Relation index, ScanKey orderby) {
    ChdbColumn* col = &chdb_search_columns(index)[orderby->sk_attno - 1];
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

    initStringInfo(&buf);
    appendStringInfo(&buf, "%s(%s, ", fn, col->name);
    append_vector(&buf, orderby->sk_argument, argtype);
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

    initStringInfo(&buf);
    initStringInfo(&where);
    if (!chdb_search_append_quals(&where, index, keys, nkeys)) {
        return NULL;
    }
    for (int i = 0; i < norderbys; i++) {
        if (orderbys[i].sk_flags & SK_ISNULL) {
            return NULL;
        }
    }

    appendStringInfoString(&buf, "SELECT ctid");
    for (int i = 0; i < norderbys; i++) {
        if (norderbys == 1) {
            appendStringInfo(
                &buf, ", %s AS _distance", chdb_search_order_expr(index, &orderbys[i])
            );
        } else {
            appendStringInfo(
                &buf,
                ", %s AS _distance%d",
                chdb_search_order_expr(index, &orderbys[i]),
                i + 1
            );
        }
    }
    appendStringInfo(&buf, " FROM %s", chdb_search_table_name(RelationGetRelid(index)));
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
