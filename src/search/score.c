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
 * statement (counts.c). The needle of each pushed text search is tokenized through
 * the store with the column's own tokenizer and preprocessor, so that the
 * tokens are the index's; the match in the SELECT list, which ClickHouse
 * evaluates without the index, names the tokenizer and applies the
 * preprocessor itself, so it agrees with the counts the index gave. A row
 * matching two rare tokens outranks one matching two common ones; a row
 * whose column is NULL scores zero for it.
 */

#include "postgres.h"

#include "fmgr.h"
#include "utils/lsyscache.h"

#include "query.h"
#include "score.h"
#include "search.h"

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

/* A (column, token) term of the expression. */
typedef struct Term {
    AttrNumber attno;
    const char* token;
} Term;

/* Whether a (column, token) term is already in the expression. */
static bool
seen(List* terms, AttrNumber attno, const char* token) {
    ListCell* lc;

    foreach (lc, terms) {
        Term* t = lfirst(lc);

        if (t->attno == attno && strcmp(t->token, token) == 0) {
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

        /*
         * The token searches, on the column asked for if one was: a regex or
         * a wildcard pattern has no tokens to weigh.
         */
        if (key->sk_strategy > CHDB_STRATEGY_HAS_PHRASE ||
            (key->sk_flags & SK_ISNULL) ||
            (col->kind != CHDB_COL_TEXT && col->kind != CHDB_COL_TEXT_ARRAY) ||
            (only && key->sk_attno != only)) {
            continue;
        }

        Oid argtype = OidIsValid(key->sk_subtype) ? key->sk_subtype : col->typid;
        Oid out;
        bool varlena;

        getTypeOutputInfo(argtype, &out, &varlena);

        const char* needle = OidOutputFunctionCall(out, key->sk_argument);
        List* tokens =
            chdb_search_score_tokens(cache, index, col, key->sk_attno, needle);

        foreach (lc, tokens) {
            const char* token = lfirst(lc);

            if (seen(terms, key->sk_attno, token)) {
                continue;
            }
            int64 rows = chdb_search_score_rows(cache, index);
            int64 df   = chdb_search_score_df(cache, index, col, key->sk_attno, token);
            Term* t    = palloc0(sizeof(*t));

            t->attno = key->sk_attno;
            t->token = token;
            terms    = lappend(terms, t);
            /* The idf as a formula over the counts, which ClickHouse folds. */
            appendStringInfo(
                &buf,
                "%slog(%.1f / %.1f + 1) * ifNull(",
                buf.len ? " + " : "",
                (double)(rows - df) + 0.5,
                (double)df + 0.5
            );
            chdb_search_score_match(
                &buf,
                chdb_search_preprocessed(col, col->name, false),
                token,
                chdb_search_tokenizer(col)
            );
            appendStringInfoString(&buf, ", 0)");
        }
    }
    return buf.len ? buf.data : pstrdup("0");
}
