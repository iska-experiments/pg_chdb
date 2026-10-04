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
 * Strategy numbers, shared with the SQL script. Text columns (text_ops,
 * text_array_ops):
 */
#define CHDB_STRATEGY_HAS_ALL_TOKENS 1 /* @@@ */
#define CHDB_STRATEGY_HAS_ANY_TOKENS 2 /* @@? */
#define CHDB_STRATEGY_HAS_TOKEN 3
#define CHDB_STRATEGY_HAS_PHRASE 4 /* @@~ */

/* Columnar columns (columnar_ops): = < <= > >= */
#define CHDB_STRATEGY_EQ 1
#define CHDB_STRATEGY_LT 2
#define CHDB_STRATEGY_LE 3
#define CHDB_STRATEGY_GT 4
#define CHDB_STRATEGY_GE 5

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
 * is negative for none. Returns NULL when a key's argument is NULL, as the
 * operators are strict and the scan can return nothing.
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
 * The pieces, for builders that select other things. The WHERE part appends
 * `a AND b` without the keyword and returns false when a key is NULL. A
 * literal is rendered for a column of ClickHouse type matching `typid`.
 */
extern bool
chdb_search_append_quals(StringInfo buf, Relation index, ScanKey keys, int nkeys);
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
chdb_search_order_expr(Relation index, ScanKey orderby);

#endif /* CHDB_SEARCH_QUERY_H */
