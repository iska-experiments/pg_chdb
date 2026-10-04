/*
 * chdb.score(k): the relevance of a row to the query's text searches, as the
 * design's "Relevance score" section defines it. The SQL function is a
 * placeholder that raises: inside a custom scan the planner replaces it with
 * a column the store computes (planner/score.c), and what the store computes
 * is rendered here. The score is an IDF-weighted overlap, since ClickHouse's
 * text index holds no term frequencies:
 *
 *   score = sum over the query's tokens t of idf(t) * [the column has t]
 *   idf(t) = log((N - df(t) + 0.5) / (df(t) + 0.5) + 1)
 *
 * with N the store table's row count and df(t) the rows whose column has
 * t, each a tiny query the text index answers alone, asked once per
 * statement. The needle of each pushed text search is tokenized through
 * the store with the column's own tokenizer and preprocessor, so that the
 * tokens are the index's; the match in the SELECT list, which ClickHouse
 * evaluates without the index, names the tokenizer and applies the
 * preprocessor itself, so it agrees with the counts the index gave. A row
 * matching two rare tokens outranks one matching two common ones; a row
 * whose column is NULL scores zero for it.
 */

#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "query.h"
#include "search.h"
#include "stream.h"

/* The placeholder: anywhere Postgres evaluates it is outside a custom scan. */
PG_FUNCTION_INFO_V1(chdb_search_score);
Datum
chdb_search_score(PG_FUNCTION_ARGS) {
    ereport(
        ERROR,
        errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        errmsg("chdb.score() needs a chdb index scan"),
        errhint(
            "Select it from one table with a chdb index, with a search predicate on "
            "an indexed text column in the WHERE clause, in a SELECT without row "
            "locks; a column named in chdb.score(k, 'col') must be such a column."
        )
    );
    PG_RETURN_NULL();
}

/* Told by the C function behind it, as columns.c tells a column's kind. */
bool
chdb_search_is_score(Oid funcid) {
    FmgrInfo finfo;

    fmgr_info(funcid, &finfo);
    return finfo.fn_addr == chdb_search_score;
}

/* ---- the counts, kept for a statement ---- */

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

    if (!chdb_search_stream_next(s, NULL) || s->nulls[0]) {
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

/* The tokens the index makes of `needle` on the column, from the store. */
static List*
needle_tokens(
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
    appendStringInfo(
        &sql, "SELECT tokens(%s, ", chdb_search_preprocess(col, lit.data, false)
    );
    chdb_search_append_string(&sql, chdb_search_tokenizer(col));
    appendStringInfoChar(&sql, ')');

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

/* `hasAllTokens(<col>, ['tok'])`: one token, as the index's, on `expr`. */
static void
append_match(
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

/* df(t) for the column, asked of the index once per statement. */
static int64
document_frequency(
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
    append_match(&where, col->name, token, NULL);

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

/* Whether a (column, token) term is already in the expression. */
static bool
seen(List* terms, AttrNumber attno, const char* token) {
    ListCell* lc;

    foreach (lc, terms) {
        Count* c = lfirst(lc);

        if (c->attno == attno && strcmp(c->token, token) == 0) {
            return true;
        }
    }
    return false;
}

char*
chdb_search_score_expr(
    Relation index,
    const ChdbColumn* cols,
    ScanKey keys,
    int nkeys,
    AttrNumber only,
    ChdbScoreCache* cache
) {
    StringInfoData buf;
    List* terms = NIL;

    initStringInfo(&buf);
    for (int i = 0; i < nkeys; i++) {
        ScanKey key           = &keys[i];
        const ChdbColumn* col = &cols[key->sk_attno - 1];
        ListCell* lc;

        /* The text searches, on the column asked for if one was. */
        if (key->sk_strategy >= CHDB_STRATEGY_EQ || (key->sk_flags & SK_ISNULL) ||
            (col->kind != CHDB_COL_TEXT && col->kind != CHDB_COL_TEXT_ARRAY) ||
            (only && key->sk_attno != only)) {
            continue;
        }

        Oid argtype = OidIsValid(key->sk_subtype) ? key->sk_subtype : col->typid;
        Oid out;
        bool varlena;

        getTypeOutputInfo(argtype, &out, &varlena);

        const char* needle = OidOutputFunctionCall(out, key->sk_argument);
        List* tokens       = needle_tokens(cache, index, col, key->sk_attno, needle);

        foreach (lc, tokens) {
            const char* token = lfirst(lc);

            if (seen(terms, key->sk_attno, token)) {
                continue;
            }
            if (cache->rows < 0) {
                cache->rows = count_rows(cache, index, NULL);
            }

            int64 df = document_frequency(cache, index, col, key->sk_attno, token);
            Count* t = palloc0(sizeof(*t));

            t->attno = key->sk_attno;
            t->token = (char*)token;
            terms    = lappend(terms, t);
            /* The idf as a formula over the counts, which ClickHouse folds. */
            appendStringInfo(
                &buf,
                "%slog(%.1f / %.1f + 1) * ifNull(",
                buf.len ? " + " : "",
                (double)(cache->rows - df) + 0.5,
                (double)df + 0.5
            );
            append_match(
                &buf,
                chdb_search_preprocess(
                    col, col->name, col->kind == CHDB_COL_TEXT_ARRAY
                ),
                token,
                chdb_search_tokenizer(col)
            );
            appendStringInfoString(&buf, ", 0)");
        }
    }
    return buf.len ? buf.data : pstrdup("0");
}
