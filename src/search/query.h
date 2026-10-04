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
#define CHDB_STRATEGY_HAS_TOKEN 3      /* @@= */
#define CHDB_STRATEGY_HAS_PHRASE 4     /* @@~ */
#define CHDB_STRATEGY_QUERY 5          /* @@@ chdb.query: a tree of the others */
#define CHDB_STRATEGY_REGEX 6          /* @@/, match() */
#define CHDB_STRATEGY_WILDCARD 7       /* @@%, LIKE */

/*
 * A search as a tree: the value of the chdb.query type, and what the
 * CustomScan hands chdb_search_render_query for the OR and NOT the access
 * method's ANDed scan keys cannot carry. The leaves are the text searches,
 * numbered by their strategy, so a leaf renders and evaluates as the
 * operator of that strategy does; boost carries a weight the filter ignores
 * and the score layer multiplies by; and, or and not combine. A leaf may
 * name an index column of its own, for a tree over several columns; one
 * that names none is on the column of the operator it is the argument of.
 *
 * On the wire (querytree.c) a tree is its preorder walk: a kind byte, then
 * for a leaf the slop (int32), the column (uint32 length, 0 for none, and
 * the bytes) and the needle (uint32 length and the bytes); for a boost the
 * weight (float4) and the child; for and and or the count (uint16) and the
 * children; for not the child. Native byte order, unaligned.
 */
typedef enum ChdbQueryKind {
    CHDB_Q_MATCH_ALL = CHDB_STRATEGY_HAS_ALL_TOKENS,
    CHDB_Q_MATCH_ANY = CHDB_STRATEGY_HAS_ANY_TOKENS,
    CHDB_Q_TERM      = CHDB_STRATEGY_HAS_TOKEN,
    CHDB_Q_PHRASE    = CHDB_STRATEGY_HAS_PHRASE,
    CHDB_Q_REGEX     = CHDB_STRATEGY_REGEX,
    CHDB_Q_WILDCARD  = CHDB_STRATEGY_WILDCARD,
    CHDB_Q_BOOST     = 8,
    CHDB_Q_AND       = 9,
    CHDB_Q_OR        = 10,
    CHDB_Q_NOT       = 11,
} ChdbQueryKind;

#define CHDB_Q_IS_LEAF(kind) ((kind) < CHDB_Q_BOOST)

typedef struct ChdbQuery {
    ChdbQueryKind kind;
    const char* needle; /* leaves */
    const char* column; /* leaves: the index column searched, NULL for the operator's */
    int32 slop;         /* phrase: tokens allowed between the needle's, in order */
    float4 weight;      /* boost */
    int nchildren;      /* and, or: at least one; boost, not: one */
    struct ChdbQuery** children;
} ChdbQuery;

/* ---- querytree.c: building, the varlena of chdb.query and back ---- */
extern ChdbQuery*
chdb_search_query_leaf(ChdbQueryKind kind, const char* needle, int32 slop);
/* and, or, not and boost over `children`, which the node keeps. */
extern ChdbQuery*
chdb_search_query_group(ChdbQueryKind kind, int nchildren, ChdbQuery** children);
extern ChdbQuery*
chdb_search_query_boost(ChdbQuery* child, float4 weight);
/* Names `column` on every leaf of `q` that names none yet. */
extern void
chdb_search_query_in_column(ChdbQuery* q, const char* column);
extern Datum
chdb_search_query_to_datum(const ChdbQuery* q);
extern ChdbQuery*
chdb_search_query_from_datum(Datum d);

/* ---- queryio.c: the readable form, which the type reads and prints ---- */
extern char*
chdb_search_query_to_string(const ChdbQuery* q);
extern ChdbQuery*
chdb_search_query_from_string(const char* s);

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
 *
 * A transaction sees its own rows: when it has rows buffered or staged for
 * the index, the query becomes a UNION ALL of the same SELECT over the
 * table and over the staging table, ordered and limited again outside
 * (chdb_search_append_from below).
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
 * ` FROM <table>[ WHERE <where>]`, where every statement over the index's
 * rows reads them, and false. When the transaction has rows buffered or
 * staged for the index (chdb_search_staged_table, below), the buffered
 * ones are staged first and the FROM clause is instead `(<leg> UNION ALL
 * <leg>)`, the legs `<list> FROM <table>[ WHERE <where>]<tail>` over the
 * table and over the staging table, whose WHERE also hides the rows of
 * savepoints rolled back since; it then returns true. `list` and `tail`
 * default to `SELECT *` and nothing: a statement whose legs order and limit
 * their own rows, as the HNSW index serves only an ORDER BY ... LIMIT on the
 * table it reads, gives its own, and merges the legs by what they select.
 */
extern bool
chdb_search_append_from(
    StringInfo buf,
    Relation index,
    const char* where,
    const char* list,
    const char* tail
);
/*
 * The staging table the FROM clause reads besides the index's table, or
 * NULL (staging.c): the transaction's rows buffered so far are staged first.
 * *excluded is the `xmin NOT IN` list that hides the rows of savepoints
 * since rolled back, or NULL.
 */
extern char*
chdb_search_staged_table(Relation index, const char** excluded);

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
/*
 * `hasAllTokens(col, 'needle')` or the like: a text search of a column by
 * the strategy of its operator; `slop` is a phrase's.
 */
extern void
chdb_search_append_text_search(
    StringInfo buf,
    const struct ChdbColumn* col,
    StrategyNumber strategy,
    const char* needle,
    int32 slop
);
/*
 * A query tree as one ClickHouse boolean expression over the index's
 * columns, for the WHERE clause: `attno` is the column of a leaf that names
 * none, or 0 when every leaf names its own. With a column, the expression
 * is false for a NULL text as the strict operator is (a NOT would otherwise
 * admit it); with 0 the caller owns the NULL semantics. Boost weights are
 * ignored here; the filter decides which rows match, the score (score.c)
 * how well.
 */
/*
 * The column a leaf searches: the one it names, else column `attno`, the
 * operator's. Raises for a name that is no text column of the index.
 */
extern const struct ChdbColumn*
chdb_search_leaf_column(
    const struct ChdbColumn* cols,
    int natts,
    int attno,
    const ChdbQuery* q
);
extern void
chdb_search_render_query(
    StringInfo buf,
    const struct ChdbColumn* cols,
    int natts,
    int attno,
    const ChdbQuery* q
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

/*
 * The index's tokenizer and preprocessor as expressions (textindex.c), for
 * the searches that read the text or its tokens rather than the posting
 * lists, and the phrase with slop built from them (phrase.c).
 */
/* The column's tokenizer as the DDL and a text search spell it: `ngrams(3)`. */
extern char*
chdb_search_tokenizer(const struct ChdbColumn* column);
/*
 * The column's preprocessor applied to `expr`: the column itself, or with
 * `literal` a string literal, for a needle to be compared with the column's
 * preprocessed text. The column as is when the preprocessor is none.
 */
extern char*
chdb_search_preprocessed(
    const struct ChdbColumn* column,
    const char* expr,
    bool literal
);
/* Whether the column's preprocessor folds case, so a search of it must too. */
extern bool
chdb_search_folds_case(const struct ChdbColumn* column);
/*
 * `tokens(<preprocessed expr>, '<tokenizer>'[, <args>])`: the tokens the
 * column's index derives from the column or, with `literal`, from a string,
 * for a phrase's positions and the score's needles.
 */
extern char*
chdb_search_tokens_call(
    const struct ChdbColumn* column,
    const char* expr,
    bool literal
);

/* A phrase whose tokens may be `slop` others apart, from token positions. */
extern void
chdb_search_append_slop_phrase(
    StringInfo buf,
    const struct ChdbColumn* column,
    const char* needle,
    int32 slop
);

#endif /* CHDB_SEARCH_QUERY_H */
