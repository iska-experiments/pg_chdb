#ifndef CHDB_SEARCH_SCORE_H
#define CHDB_SEARCH_SCORE_H

/*
 * chdb.score(): the placeholder and the expression the store computes for
 * it (score.c), from the counts the store answers once per statement
 * (counts.c). The planner binds the calls (planner/score.c) and keeps a
 * cache of counts per statement.
 */

#include "postgres.h"

#include "access/skey.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/rel.h"

struct ChdbColumn;

/* ---- score.c ---- */
/* chdb.score(): a placeholder that raises wherever Postgres evaluates it. */
extern PGDLLEXPORT Datum chdb_search_score(PG_FUNCTION_ARGS);
/* Whether `funcid` is chdb.score(), told by the C function behind it. */
extern bool
chdb_search_is_score(Oid funcid);
typedef struct ChdbScoreCache ChdbScoreCache;
/*
 * The ClickHouse expression of chdb.score() over the text searches among
 * `keys`, on index column `only` or on every text column when it is zero:
 * the sum over their needles' tokens of idf(token) times whether the column
 * has it, tokenized and counted through the store, asked once per cache.
 */
extern char*
chdb_search_score_expr(
    Relation index,
    const struct ChdbColumn* cols,
    ScanKey keys,
    int nkeys,
    AttrNumber only,
    ChdbScoreCache* cache
);

/* ---- counts.c ---- */
/* The counts the score asked of the store, kept for a statement. */
extern ChdbScoreCache*
chdb_search_score_cache(MemoryContext cxt);
/* The tokens the index makes of `needle` on the column, from the store. */
extern List*
chdb_search_score_tokens(
    ChdbScoreCache* cache,
    Relation index,
    const struct ChdbColumn* col,
    AttrNumber attno,
    const char* needle
);
/* N, the rows a search of the index reads. */
extern int64
chdb_search_score_rows(ChdbScoreCache* cache, Relation index);
/* df(t), the rows whose column `attno` has `token`, as the index answers it. */
extern int64
chdb_search_score_df(
    ChdbScoreCache* cache,
    Relation index,
    const struct ChdbColumn* col,
    AttrNumber attno,
    const char* token
);
/*
 * `hasAllTokens(<expr>, ['tok'][, '<tokenizer>'])`: one token, as the
 * index's, on `expr`; the tokenizer for an expression the index does not
 * serve, which ClickHouse would otherwise split by its default.
 */
extern void
chdb_search_score_match(
    StringInfo buf,
    const char* expr,
    const char* token,
    const char* tokenizer
);

#endif /* CHDB_SEARCH_SCORE_H */
