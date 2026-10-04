/*
 * The chdb.query type's tree (query.h) and its varlena, the tree's preorder
 * walk. querybuild.c has the functions of schema chdb that build trees,
 * queryio.c reads and prints the readable form, queryeval.c is the Postgres
 * fallback and query.c renders a tree for ClickHouse.
 */

#include "postgres.h"

#include <string.h>

#include "fmgr.h"
#include "utils/builtins.h"

#include "query.h"
#include "search.h"

ChdbQuery*
chdb_search_query_leaf(ChdbQueryKind kind, const char* needle, int32 slop) {
    ChdbQuery* q = palloc0(sizeof(*q));

    if (slop < 0) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("a phrase's slop cannot be negative")
        );
    }
    q->kind   = kind;
    q->needle = needle;
    q->slop   = slop;
    return q;
}

/* A group of one and or or is that child. */
ChdbQuery*
chdb_search_query_group(ChdbQueryKind kind, int nchildren, ChdbQuery** children) {
    ChdbQuery* q = palloc0(sizeof(*q));

    if (nchildren < 1) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("a chdb.query combination needs at least one query")
        );
    }
    if (nchildren == 1 && (kind == CHDB_Q_AND || kind == CHDB_Q_OR)) {
        return children[0];
    }
    q->kind      = kind;
    q->nchildren = nchildren;
    q->children  = children;
    return q;
}

ChdbQuery*
chdb_search_query_boost(ChdbQuery* child, float4 weight) {
    ChdbQuery** children = palloc(sizeof(ChdbQuery*));
    ChdbQuery* q;

    children[0] = child;
    q           = chdb_search_query_group(CHDB_Q_BOOST, 1, children);
    q->weight   = weight;
    return q;
}

void
chdb_search_query_in_column(ChdbQuery* q, const char* column) {
    if (CHDB_Q_IS_LEAF(q->kind)) {
        if (!q->column) {
            q->column = column;
        }
        return;
    }
    for (int i = 0; i < q->nchildren; i++) {
        chdb_search_query_in_column(q->children[i], column);
    }
}

/* ---- the varlena ---- */

static void
put_string(StringInfo buf, const char* s) {
    size_t n = s ? strlen(s) : 0;
    uint32 len;

    if (n > PG_UINT32_MAX) {
        ereport(
            ERROR,
            errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
            errmsg("a chdb.query string is too long")
        );
    }
    len = (uint32)n;
    appendBinaryStringInfo(buf, &len, sizeof(len));
    appendBinaryStringInfo(buf, s, n);
}

static void
serialize(StringInfo buf, const ChdbQuery* q) {
    uint8 kind = (uint8)q->kind;

    appendBinaryStringInfo(buf, &kind, 1);
    if (CHDB_Q_IS_LEAF(q->kind)) {
        appendBinaryStringInfo(buf, &q->slop, sizeof(q->slop));
        put_string(buf, q->column);
        put_string(buf, q->needle);
        return;
    }
    if (q->kind == CHDB_Q_BOOST) {
        appendBinaryStringInfo(buf, &q->weight, sizeof(q->weight));
    } else if (q->kind != CHDB_Q_NOT) {
        uint16 n = (uint16)q->nchildren;

        appendBinaryStringInfo(buf, &n, sizeof(n));
    }
    for (int i = 0; i < q->nchildren; i++) {
        serialize(buf, q->children[i]);
    }
}

Datum
chdb_search_query_to_datum(const ChdbQuery* q) {
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfoSpaces(&buf, VARHDRSZ);
    serialize(&buf, q);
    SET_VARSIZE(buf.data, buf.len);
    return PointerGetDatum(buf.data);
}

typedef struct Cursor {
    const char* p;
    const char* end;
} Cursor;

/* The next `n` bytes, which must be there: the value is ours or corrupt. */
static const char*
take(Cursor* c, size_t n) {
    const char* at = c->p;

    if (n > (size_t)(c->end - c->p)) {
        ereport(
            ERROR, errcode(ERRCODE_DATA_CORRUPTED), errmsg("corrupt chdb.query value")
        );
    }
    c->p += n;
    return at;
}

#define TAKE(c, var) memcpy(&(var), take((c), sizeof(var)), sizeof(var))

/* NULL for an empty column name, "" for an empty needle. */
static char*
take_string(Cursor* c, bool empty_is_null) {
    uint32 n;

    TAKE(c, n);
    return n || !empty_is_null ? pnstrdup(take(c, n), n) : NULL;
}

static ChdbQuery*
deserialize(Cursor* c) {
    ChdbQuery* q = palloc0(sizeof(*q));
    uint8 kind;

    TAKE(c, kind);
    if (kind == 0 || kind == CHDB_STRATEGY_QUERY || kind > CHDB_Q_NOT) {
        take(c, (size_t)-1); /* no such node */
    }
    q->kind = kind;
    if (CHDB_Q_IS_LEAF(kind)) {
        TAKE(c, q->slop);
        q->column = take_string(c, true);
        q->needle = take_string(c, false);
        return q;
    }
    q->nchildren = 1;
    if (kind == CHDB_Q_BOOST) {
        TAKE(c, q->weight);
    } else if (kind != CHDB_Q_NOT) {
        uint16 n;

        TAKE(c, n);
        q->nchildren = n;
    }
    q->children = palloc(sizeof(ChdbQuery*) * q->nchildren);
    for (int i = 0; i < q->nchildren; i++) {
        q->children[i] = deserialize(c);
    }
    return q;
}

ChdbQuery*
chdb_search_query_from_datum(Datum d) {
    bytea* v     = PG_DETOAST_DATUM(d);
    Cursor c     = { VARDATA_ANY(v), VARDATA_ANY(v) + VARSIZE_ANY_EXHDR(v) };
    ChdbQuery* q = deserialize(&c);

    if (c.p != c.end) {
        take(&c, (size_t)-1); /* trailing bytes */
    }
    return q;
}
