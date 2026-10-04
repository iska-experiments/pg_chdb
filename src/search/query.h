#ifndef CHDB_SEARCH_QUERY_H
#define CHDB_SEARCH_QUERY_H

/*
 * ClickHouse SELECT generation for chdb indexes, shared by the index AM and
 * the CustomScan. Everything returned is palloc'd in the caller's context.
 */

#include "postgres.h"

#include "access/skey.h"
#include "lib/stringinfo.h"
#include "utils/rel.h"

/*
 * Strategy numbers, shared with the SQL script. The two sets are disjoint,
 * so a scan key is rendered by its number alone, whatever operator class it
 * came from. Text searches (text_ops, text_array_ops):
 */
#define CHDB_STRATEGY_HAS_ALL_TOKENS 1 /* @@@ */
#define CHDB_STRATEGY_HAS_ANY_TOKENS 2 /* @@? */
#define CHDB_STRATEGY_HAS_TOKEN 3
#define CHDB_STRATEGY_HAS_PHRASE 4 /* @@~ */

/* Comparisons (columnar_ops): = < <= > >= */
#define CHDB_STRATEGY_EQ 11
#define CHDB_STRATEGY_LT 12
#define CHDB_STRATEGY_LE 13
#define CHDB_STRATEGY_GT 14
#define CHDB_STRATEGY_GE 15

/* Distance ORDER BY operators, numbered as pgvector does: <-> <#> <=> <+>. */
#define CHDB_ORDER_L2 1
#define CHDB_ORDER_NEG_INNER_PRODUCT 2
#define CHDB_ORDER_COSINE 3
#define CHDB_ORDER_L1 4

/*
 * Builds
 *
 *   SELECT ctid[, <distance> AS _distance] FROM idx_<oid>.t_<generation>
 *   [WHERE <keys>] [ORDER BY _distance] [LIMIT n]
 *
 * from the scan keys and order-by keys an index scan receives. Several
 * order-bys select _distance1, _distance2 and so on. Keys are ANDed. `limit`
 * is negative for none. Returns NULL when a search key's argument is NULL,
 * as the operators are strict and the scan can return nothing; a NULL
 * order-by argument instead gives every row a NULL distance. One order-by
 * on a vector column becomes the search ClickHouse's HNSW index serves,
 * whose ORDER BY, LIMIT and SETTINGS vector.c renders.
 */
extern char*
chdb_search_build_select(
    Relation index,
    ScanKey keys,
    int nkeys,
    ScanKey orderbys,
    int norderbys,
    int64 limit
);

/*
 * The custom scan's form of the same statement, with the columns it has the
 * store compute after the distances: `scores` are ClickHouse expressions,
 * selected `AS _score` (`_score1`, `_score2`... for several) cast to Float32,
 * the type chdb.score() returns. `score_order`, 1-based, names the one the
 * rows are ordered by, descending with ties broken by ctid, in place of the
 * distances' order, which is not pushed with it; zero orders by the
 * distances. `cols` is the index's column array from chdb_search_columns,
 * computed once per statement.
 */
struct ChdbColumn;
extern char*
chdb_search_build_scored_select(
    Relation index,
    const struct ChdbColumn* cols,
    ScanKey keys,
    int nkeys,
    ScanKey orderbys,
    int norderbys,
    const char* const* scores,
    int nscores,
    int score_order,
    int64 limit
);

/*
 * The pieces, for builders that select other things. The WHERE part appends
 * `a AND b` without the keyword and returns false when a key is NULL. A
 * literal is rendered for a column of ClickHouse type matching `typid`.
 */
extern bool
chdb_search_append_quals(
    StringInfo buf,
    const struct ChdbColumn* cols,
    int natts,
    ScanKey keys,
    int nkeys
);
extern void
chdb_search_append_literal(StringInfo buf, Datum value, Oid typid);
extern void
chdb_search_append_string(StringInfo buf, const char* s);
extern void
chdb_search_append_vector(StringInfo buf, Datum value, Oid typid);
/*
 * A whole predicate for a constant that compares unlike its literal: a float
 * NaN (equal to itself and above everything in Postgres) or an infinite
 * date, timestamp or numeric (which the store cannot hold). Appends
 * isNaN(col), `col IS NOT NULL` or 0 as the strategy requires and returns
 * true; false leaves an ordinary constant to chdb_search_append_literal.
 */
extern bool
chdb_search_append_special(
    StringInfo buf,
    const char* col,
    StrategyNumber strategy,
    Datum value,
    Oid typid
);
extern char*
chdb_search_order_expr(Relation index, const struct ChdbColumn* cols, ScanKey orderby);

#endif /* CHDB_SEARCH_QUERY_H */
