/*
 * Postgres constants as ClickHouse literals, for the WHERE and ORDER BY
 * clauses query.c builds. Strings are quoted with ClickHouse's own escaping
 * (backslash and quote), so a search string cannot break out of the
 * statement.
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
