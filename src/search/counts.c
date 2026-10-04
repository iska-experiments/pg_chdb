/*
 * The counts behind chdb.score(): N, the rows a search of the index reads,
 * df(t), the rows whose column has the token t, and the tokens the index
 * makes of a needle, each a tiny query the store answers, asked once per
 * statement and kept in the cache the scan holds for it (score.c says what
 * the score makes of them). The counts read the rows through the FROM
 * clause every statement over the index takes, the transaction's staged
 * rows included, and df(t) is answered by the text index alone.
 */

#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "query.h"
#include "score.h"
#include "search.h"
#include "stream.h"

typedef struct Needle {
    AttrNumber attno;
    char* needle;
    List* tokens; /* char*, as the store tokenized it */
} Needle;

typedef struct Count {
    AttrNumber attno;
    char* token;
    int64 df;
} Count;

struct ChdbScoreCache {
    MemoryContext cxt;
    int64 rows; /* N, negative until asked */
    List* needles;
    List* counts;
};

ChdbScoreCache*
chdb_search_score_cache(MemoryContext cxt) {
    ChdbScoreCache* cache = MemoryContextAllocZero(cxt, sizeof(*cache));

    cache->cxt  = cxt;
    cache->rows = -1;
    return cache;
}

/*
 * Runs `sql`, a query of one row and one column of type `typid`, in a
 * context of its own, and returns the value copied into the cache's.
 */
static Datum
ask(ChdbScoreCache* cache,
    Relation index,
    uint64 generation,
    const char* sql,
    Oid typid) {
    MemoryContext cxt = AllocSetContextCreate(
        CurrentMemoryContext, "chdb_search score query", ALLOCSET_SMALL_SIZES
    );
    ChdbStream* s = chdb_search_stream_query(
        RelationGetRelid(index), generation, sql, &typid, 1, cxt
    );
    int16 typlen;
    bool typbyval;
    Datum value;

    if (!chdb_search_stream_next(s, NULL, NULL) || s->nulls[0]) {
        ereport(
            ERROR,
            errcode(ERRCODE_DATA_EXCEPTION),
            errmsg(
                "chdb_search: the store did not answer: %s", chdb_search_mask_sql(sql)
            )
        );
    }
    get_typlenbyval(typid, &typlen, &typbyval);

    MemoryContext old = MemoryContextSwitchTo(cache->cxt);

    value = datumCopy(s->vals[0], typbyval, typlen);
    MemoryContextSwitchTo(old);
    chdb_search_stream_close(s);
    MemoryContextDelete(cxt);
    return value;
}

List*
chdb_search_score_tokens(
    ChdbScoreCache* cache,
    Relation index,
    const ChdbColumn* col,
    AttrNumber attno,
    const char* needle
) {
    ListCell* lc;
    StringInfoData sql, lit;
    Datum* elems;
    bool* nulls;
    int n;

    foreach (lc, cache->needles) {
        Needle* entry = lfirst(lc);

        if (entry->attno == attno && strcmp(entry->needle, needle) == 0) {
            return entry->tokens;
        }
    }
    initStringInfo(&lit);
    chdb_search_append_string(&lit, needle);
    initStringInfo(&sql);
    appendStringInfo(&sql, "SELECT %s", chdb_search_tokens_call(col, lit.data, true));

    /* tokens() is tied to no table, so the request names no generation. */
    ArrayType* arr = DatumGetArrayTypeP(ask(cache, index, 0, sql.data, TEXTARRAYOID));
    MemoryContext old = MemoryContextSwitchTo(cache->cxt);
    Needle* entry     = palloc0(sizeof(*entry));

    entry->attno  = attno;
    entry->needle = pstrdup(needle);
    deconstruct_array_builtin(arr, TEXTOID, &elems, &nulls, &n);
    for (int i = 0; i < n; i++) {
        if (!nulls[i]) {
            entry->tokens = lappend(entry->tokens, TextDatumGetCString(elems[i]));
        }
    }
    cache->needles = lappend(cache->needles, entry);
    MemoryContextSwitchTo(old);
    return entry->tokens;
}

/*
 * `count()` of the store's rows, under `where` if given, as the index
 * answers it: with the transaction's staged rows, which the scan reads too.
 */
static int64
count_rows(ChdbScoreCache* cache, Relation index, const char* where) {
    StringInfoData sql;

    initStringInfo(&sql);
    appendStringInfoString(&sql, "SELECT count()");
    chdb_search_append_from(&sql, index, where ? where : "", NULL, NULL);
    return DatumGetInt64(
        ask(cache, index, chdb_meta_generation(index), sql.data, INT8OID)
    );
}

void
chdb_search_score_match(
    StringInfo buf,
    const char* expr,
    const char* token,
    const char* tokenizer
) {
    appendStringInfo(buf, "hasAllTokens(%s, [", expr);
    chdb_search_append_string(buf, token);
    appendStringInfoChar(buf, ']');
    if (tokenizer) {
        appendStringInfoString(buf, ", ");
        chdb_search_append_string(buf, tokenizer);
    }
    appendStringInfoChar(buf, ')');
}

int64
chdb_search_score_df(
    ChdbScoreCache* cache,
    Relation index,
    const ChdbColumn* col,
    AttrNumber attno,
    const char* token
) {
    ListCell* lc;
    StringInfoData where;

    foreach (lc, cache->counts) {
        Count* c = lfirst(lc);

        if (c->attno == attno && strcmp(c->token, token) == 0) {
            return c->df;
        }
    }
    initStringInfo(&where);
    chdb_search_score_match(&where, col->name, token, NULL);

    int64 df          = count_rows(cache, index, where.data);
    MemoryContext old = MemoryContextSwitchTo(cache->cxt);
    Count* c          = palloc0(sizeof(*c));

    c->attno      = attno;
    c->token      = pstrdup(token);
    c->df         = df;
    cache->counts = lappend(cache->counts, c);
    MemoryContextSwitchTo(old);
    return df;
}

int64
chdb_search_score_rows(ChdbScoreCache* cache, Relation index) {
    if (cache->rows < 0) {
        cache->rows = count_rows(cache, index, NULL);
    }
    return cache->rows;
}
