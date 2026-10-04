/*
 * The readable form of a chdb.query, which its output function prints and
 * its input function reads back: each node as a call of the builder that
 * makes it, with the needle quoted as SQL quotes a string:
 *
 *   and(match_all('running shoes'), not(phrase('on sale', 1)), boost(term('new'), 2))
 *   in_column(wildcard('run%'), 'title')
 *
 * It is what EXPLAIN shows for a query the planner folded to a constant,
 * and a way to write one in a prepared statement's parameter.
 */

#include "postgres.h"

#include <ctype.h>
#include <string.h>

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"

#include "query.h"
#include "search.h"

static const struct {
    const char* name;
    ChdbQueryKind kind;
} names[] = {
    { "match_all", CHDB_Q_MATCH_ALL },
    { "match",     CHDB_Q_MATCH_ANY },
    { "term",      CHDB_Q_TERM      },
    { "phrase",    CHDB_Q_PHRASE    },
    { "regex",     CHDB_Q_REGEX     },
    { "wildcard",  CHDB_Q_WILDCARD  },
    { "boost",     CHDB_Q_BOOST     },
    { "and",       CHDB_Q_AND       },
    { "or",        CHDB_Q_OR        },
    { "not",       CHDB_Q_NOT       },
};

static const char*
name_of(ChdbQueryKind kind) {
    for (int i = 0; i < lengthof(names); i++) {
        if (names[i].kind == kind) {
            return names[i].name;
        }
    }
    elog(ERROR, "unknown chdb.query node kind %d", kind);
}

/* ---- printing ---- */

static void
quote(StringInfo buf, const char* s) {
    appendStringInfoChar(buf, '\'');
    for (; *s; s++) {
        if (*s == '\'') {
            appendStringInfoChar(buf, '\'');
        }
        appendStringInfoChar(buf, *s);
    }
    appendStringInfoChar(buf, '\'');
}

static void
print(StringInfo buf, const ChdbQuery* q) {
    if (CHDB_Q_IS_LEAF(q->kind)) {
        if (q->column) {
            appendStringInfoString(buf, "in_column(");
        }
        appendStringInfo(buf, "%s(", name_of(q->kind));
        quote(buf, q->needle);
        if (q->kind == CHDB_Q_PHRASE && q->slop) {
            appendStringInfo(buf, ", %d", q->slop);
        }
        appendStringInfoChar(buf, ')');
        if (q->column) {
            appendStringInfoString(buf, ", ");
            quote(buf, q->column);
            appendStringInfoChar(buf, ')');
        }
        return;
    }
    appendStringInfo(buf, "%s(", name_of(q->kind));
    for (int i = 0; i < q->nchildren; i++) {
        if (i) {
            appendStringInfoString(buf, ", ");
        }
        print(buf, q->children[i]);
    }
    if (q->kind == CHDB_Q_BOOST) {
        appendStringInfo(
            buf,
            ", %s",
            DatumGetCString(DirectFunctionCall1(float4out, Float4GetDatum(q->weight)))
        );
    }
    appendStringInfoChar(buf, ')');
}

char*
chdb_search_query_to_string(const ChdbQuery* q) {
    StringInfoData buf;

    initStringInfo(&buf);
    print(&buf, q);
    return buf.data;
}

/* ---- parsing ---- */

typedef struct Parser {
    const char* start;
    const char* p;
} Parser;

static void
fail(Parser* ps, const char* what) {
    ereport(
        ERROR,
        errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
        errmsg("invalid input syntax for type chdb.query: \"%s\"", ps->start),
        errdetail("Expected %s at character %d.", what, (int)(ps->p - ps->start) + 1)
    );
}

static void
skip_spaces(Parser* ps) {
    while (isspace((unsigned char)*ps->p)) {
        ps->p++;
    }
}

static bool
accept(Parser* ps, char c) {
    skip_spaces(ps);
    if (*ps->p != c) {
        return false;
    }
    ps->p++;
    return true;
}

static void
expect(Parser* ps, char c) {
    if (!accept(ps, c)) {
        fail(ps, psprintf("\"%c\"", c));
    }
}

/* A SQL string literal, with '' for a quote. */
static char*
parse_string(Parser* ps) {
    StringInfoData buf;

    expect(ps, '\'');
    initStringInfo(&buf);
    for (;; ps->p++) {
        if (*ps->p == '\0') {
            fail(ps, "the closing quote");
        }
        if (*ps->p == '\'') {
            if (ps->p[1] != '\'') {
                break;
            }
            ps->p++;
        }
        appendStringInfoChar(&buf, *ps->p);
    }
    ps->p++;
    return buf.data;
}

/* The characters up to the next separator, for a number. */
static char*
parse_number(Parser* ps) {
    const char* start;

    skip_spaces(ps);
    start = ps->p;
    while (*ps->p && !strchr(",) \t\n", *ps->p)) {
        ps->p++;
    }
    if (ps->p == start) {
        fail(ps, "a number");
    }
    return pnstrdup(start, ps->p - start);
}

static ChdbQuery*
parse_query(Parser* ps) {
    const char* start;
    char* name;
    ChdbQuery* q;

    skip_spaces(ps);
    start = ps->p;
    while (isalpha((unsigned char)*ps->p) || *ps->p == '_') {
        ps->p++;
    }
    name = pnstrdup(start, ps->p - start);
    expect(ps, '(');

    if (strcmp(name, "in_column") == 0) {
        q = parse_query(ps);
        expect(ps, ',');
        chdb_search_query_in_column(q, parse_string(ps));
        expect(ps, ')');
        return q;
    }

    int i = 0;

    while (i < lengthof(names) && strcmp(names[i].name, name) != 0) {
        i++;
    }
    if (i == lengthof(names)) {
        ps->p = start;
        fail(ps, "a query function name");
    }

    ChdbQueryKind kind = names[i].kind;

    if (CHDB_Q_IS_LEAF(kind)) {
        char* needle = parse_string(ps);
        int32 slop   = 0;

        if (kind == CHDB_Q_PHRASE && accept(ps, ',')) {
            slop = pg_strtoint32(parse_number(ps));
        }
        q = chdb_search_query_leaf(kind, needle, slop);
    } else {
        int n = 0, cap = 4;
        ChdbQuery** children = palloc(sizeof(ChdbQuery*) * cap);

        do {
            if (kind == CHDB_Q_BOOST && n == 1) {
                break;
            }
            if (n == cap) {
                children = repalloc(children, sizeof(ChdbQuery*) * (cap *= 2));
            }
            children[n++] = parse_query(ps);
        } while (accept(ps, ','));
        q = chdb_search_query_group(kind, n, children);
        if (kind == CHDB_Q_BOOST) {
            q->weight = DatumGetFloat4(
                DirectFunctionCall1(float4in, CStringGetDatum(parse_number(ps)))
            );
        } else if (kind == CHDB_Q_NOT && n != 1) {
            fail(ps, "one query in not()");
        }
    }
    expect(ps, ')');
    return q;
}

ChdbQuery*
chdb_search_query_from_string(const char* s) {
    Parser ps    = { s, s };
    ChdbQuery* q = parse_query(&ps);

    skip_spaces(&ps);
    if (*ps.p) {
        fail(&ps, "the end of the query");
    }
    return q;
}

PG_FUNCTION_INFO_V1(chdb_search_query_in);
Datum
chdb_search_query_in(PG_FUNCTION_ARGS) {
    return chdb_search_query_to_datum(
        chdb_search_query_from_string(PG_GETARG_CSTRING(0))
    );
}

PG_FUNCTION_INFO_V1(chdb_search_query_out);
Datum
chdb_search_query_out(PG_FUNCTION_ARGS) {
    PG_RETURN_CSTRING(
        chdb_search_query_to_string(chdb_search_query_from_datum(PG_GETARG_DATUM(0)))
    );
}
