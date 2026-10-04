/*
 * Postgres constants as ClickHouse literals, for the WHERE and ORDER BY
 * clauses query.c builds. Strings are quoted with ClickHouse's own escaping
 * (backslash and quote), numbers are parsed to the column's own type, and
 * the values ClickHouse compares differently from Postgres, NaN and the
 * infinities, become predicates of their own.
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
        /*
         * Parsed to the column's own width: a bare 0.1 is a Float64, which
         * the Float32 nearest to 0.1 does not equal. ClickHouse reads
         * Infinity and NaN from the text as Postgres writes them.
         */
        appendStringInfo(buf, "toFloat32('%s')", output_of(value, typid));
        return;
    case FLOAT8OID:
        appendStringInfo(buf, "toFloat64('%s')", output_of(value, typid));
        return;
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
void
chdb_search_append_vector(StringInfo buf, Datum value, Oid typid) {
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

/*
 * Constants that compare unlike their ClickHouse rendering: a float NaN,
 * which Postgres treats as equal to itself and above every number while
 * ClickHouse's nan equals nothing, and the infinities of date, timestamp and
 * numeric, which the store cannot hold (the writer refuses them) and which
 * append_literal would reject at execution.
 */
typedef enum SpecialValue {
    ORDINARY,
    FLOAT_NAN, /* stored, equal to itself, above every other value */
    ABOVE_ALL, /* unstorable, above every stored value */
    BELOW_ALL, /* unstorable, below every stored value */
} SpecialValue;

static SpecialValue
classify(Datum value, Oid typid) {
    switch (typid) {
    case FLOAT4OID:
        return isnan(DatumGetFloat4(value)) ? FLOAT_NAN : ORDINARY;
    case FLOAT8OID:
        return isnan(DatumGetFloat8(value)) ? FLOAT_NAN : ORDINARY;
    case DATEOID: {
        DateADT d = DatumGetDateADT(value);

        return DATE_IS_NOEND(d) ? ABOVE_ALL : DATE_IS_NOBEGIN(d) ? BELOW_ALL : ORDINARY;
    }
    case TIMESTAMPOID:
    case TIMESTAMPTZOID: {
        Timestamp t = DatumGetTimestamp(value);

        return TIMESTAMP_IS_NOEND(t)     ? ABOVE_ALL
               : TIMESTAMP_IS_NOBEGIN(t) ? BELOW_ALL
                                         : ORDINARY;
    }
    case NUMERICOID: {
        char* s = output_of(value, typid);

        if (strcmp(s, "NaN") == 0 || strcmp(s, "Infinity") == 0) {
            return ABOVE_ALL;
        }
        return strcmp(s, "-Infinity") == 0 ? BELOW_ALL : ORDINARY;
    }
    default:
        return ORDINARY;
    }
}

bool
chdb_search_append_special(
    StringInfo buf,
    const char* col,
    StrategyNumber strategy,
    Datum value,
    Oid typid
) {
    bool matches; /* whether any stored value satisfies the comparison */

    switch (classify(value, typid)) {
    case ORDINARY:
        return false;
    case FLOAT_NAN:
        if (strategy == CHDB_STRATEGY_EQ || strategy == CHDB_STRATEGY_GE) {
            appendStringInfo(buf, "isNaN(%s)", col);
            return true;
        }
        if (strategy == CHDB_STRATEGY_LT) {
            appendStringInfo(buf, "NOT isNaN(%s)", col);
            return true;
        }
        matches = strategy == CHDB_STRATEGY_LE; /* nothing is above NaN */
        break;
    case ABOVE_ALL:
        matches = strategy == CHDB_STRATEGY_LT || strategy == CHDB_STRATEGY_LE;
        break;
    case BELOW_ALL:
        matches = strategy == CHDB_STRATEGY_GT || strategy == CHDB_STRATEGY_GE;
        break;
    }
    /* Never a bare 1: xs_recheck is false, so NULLs would be returned. */
    if (matches) {
        appendStringInfo(buf, "%s IS NOT NULL", col);
    } else {
        appendStringInfoChar(buf, '0');
    }
    return true;
}
