/*
 * chdb.score(k): the relevance of a row to the query's text searches, as the
 * design's "Relevance score" section defines it. The SQL function is a
 * placeholder that raises: inside a custom scan the planner replaces it with
 * a column the store computes (planner/score.c), and what the store computes
 * is rendered here. The score is an IDF-weighted overlap, since ClickHouse's
 * text index holds no term frequencies:
 *
 *   score = sum over the query's tokens t of w(t) * idf(t) * [the column has t]
 *   idf(t) = log((N - df(t) + 0.5) / (df(t) + 0.5) + 1)
 *
 * with N the store table's row count and df(t) the rows whose column has t,
 * each a tiny query the text index answers alone, asked once per statement
 * (counts.c), and w(t) the product of the boosts above the chdb.query leaf
 * t came from, 1 for an operator's needle. The needle of each pushed token
 * search, and of each token leaf of a tree, is tokenized through the store
 * with the column's own tokenizer and preprocessor, so that the tokens are
 * the index's; the match in the SELECT list, which ClickHouse evaluates
 * without the index, names the tokenizer and applies the preprocessor
 * itself, so it agrees with the counts the index gave. A row matching two
 * rare tokens outranks one matching two common ones; a row whose column is
 * NULL scores zero for it.
 */

#include "postgres.h"

#include <string.h>

#include "fmgr.h"
#include "utils/builtins.h"

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

/* A needle of the scan's token searches, with the weight its boosts give it. */
typedef struct Search {
    AttrNumber attno;
    const char* needle;
    double weight;
} Search;

/* A (column, token) term of the expression, with the largest weight given it. */
typedef struct Term {
    AttrNumber attno;
    const char* token;
    double weight;
} Term;

static List*
add_search(List* searches, AttrNumber attno, const char* needle, double weight) {
    Search* s = palloc(sizeof(*s));

    s->attno  = attno;
    s->needle = needle;
    s->weight = weight;
    return lappend(searches, s);
}

/*
 * The token leaves of a chdb.query, on the key's column or the one a leaf
 * names, each weighed by the product of the boosts above it. A pattern has
 * no tokens to weigh, and a leaf under a NOT counts for no row the tree
 * matches.
 */
static List*
tree_searches(
    List* searches,
    const ChdbColumn* cols,
    int natts,
    AttrNumber attno,
    const ChdbQuery* q,
    double weight
) {
    switch (q->kind) {
    case CHDB_Q_BOOST:
        return tree_searches(
            searches, cols, natts, attno, q->children[0], weight * q->weight
        );
    case CHDB_Q_AND:
    case CHDB_Q_OR:
        for (int i = 0; i < q->nchildren; i++) {
            searches =
                tree_searches(searches, cols, natts, attno, q->children[i], weight);
        }
        return searches;
    case CHDB_Q_NOT:
    case CHDB_Q_REGEX:
    case CHDB_Q_WILDCARD:
        return searches;
    default:
        return add_search(
            searches,
            chdb_search_leaf_column(cols, natts, attno, q) - cols + 1,
            q->needle,
            weight
        );
    }
}

/* Adds the term, or raises the weight of the one already there to `weight`. */
static List*
add_term(List* terms, AttrNumber attno, const char* token, double weight) {
    ListCell* lc;
    Term* t;

    foreach (lc, terms) {
        t = lfirst(lc);
        if (t->attno == attno && strcmp(t->token, token) == 0) {
            t->weight = Max(t->weight, weight);
            return terms;
        }
    }
    t         = palloc(sizeof(*t));
    t->attno  = attno;
    t->token  = token;
    t->weight = weight;
    return lappend(terms, t);
}

/*
 * The needles of the token searches among the keys, a chdb.query's leaves
 * with their boosts: a regex or a wildcard pattern has no tokens to weigh.
 */
static List*
searches_of(const ChdbColumn* cols, int natts, ScanKey keys, int nkeys) {
    List* searches = NIL;

    for (int i = 0; i < nkeys; i++) {
        ScanKey key           = &keys[i];
        const ChdbColumn* col = &cols[key->sk_attno - 1];

        if ((key->sk_flags & SK_ISNULL) ||
            (col->kind != CHDB_COL_TEXT && col->kind != CHDB_COL_TEXT_ARRAY)) {
            continue;
        }
        if (key->sk_strategy <= CHDB_STRATEGY_HAS_PHRASE) {
            searches = add_search(
                searches, key->sk_attno, TextDatumGetCString(key->sk_argument), 1
            );
        } else if (key->sk_strategy == CHDB_STRATEGY_QUERY) {
            searches = tree_searches(
                searches,
                cols,
                natts,
                key->sk_attno,
                chdb_search_query_from_datum(key->sk_argument),
                1
            );
        }
    }
    return searches;
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
    ListCell *ls, *lt;

    /* The tokens first, each needle's in turn, then the counts per term. */
    foreach (ls, searches_of(cols, index->rd_att->natts, keys, nkeys)) {
        Search* s = lfirst(ls);
        List* tokens;

        if (only && s->attno != only) {
            continue;
        }
        tokens = chdb_search_score_tokens(
            cache, index, &cols[s->attno - 1], s->attno, s->needle
        );
        foreach (lt, tokens) {
            terms = add_term(terms, s->attno, lfirst(lt), s->weight);
        }
    }
    initStringInfo(&buf);
    foreach (lt, terms) {
        Term* t               = lfirst(lt);
        const ChdbColumn* col = &cols[t->attno - 1];
        int64 rows            = chdb_search_score_rows(cache, index);
        int64 df = chdb_search_score_df(cache, index, col, t->attno, t->token);

        appendStringInfoString(&buf, buf.len ? " + " : "");
        if (t->weight != 1) {
            appendStringInfo(&buf, "%g * ", t->weight);
        }
        /* The idf as a formula over the counts, which ClickHouse folds. */
        appendStringInfo(
            &buf,
            "log(%.1f / %.1f + 1) * ifNull(",
            (double)(rows - df) + 0.5,
            (double)df + 0.5
        );
        chdb_search_score_match(
            &buf,
            chdb_search_preprocessed(col, col->name, false),
            t->token,
            chdb_search_tokenizer(col)
        );
        appendStringInfoString(&buf, ", 0)");
    }
    return buf.len ? buf.data : pstrdup("0");
}
