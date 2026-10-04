/*
 * The contract between the chdb_vector extension and the chdb access method
 * (and the CustomScan). The AM reads it from the catalogs and calls the
 * support functions; it never links against chdb_vector.
 *
 * Each pgvector `vector` column indexed by a chdb opclass becomes:
 *
 *   - a column `<name> Array(Float32)` in the ClickHouse table
 *     (CHDB_VECTOR_CH_TYPE). The encoder casts each vector through
 *     `vector::real[]` (pgvector's implicit cast, vector_to_float4) and
 *     writes the elements as Float32. ClickHouse wants every array at the
 *     index's dimension, so a NULL vector cannot be stored: the AM refuses
 *     the row.
 *   - a skip index (see CHDB_VECTOR_INDEX_TYPE).
 *
 * An opclass is recognized as a vector opclass when it has support function
 * CHDB_VECTOR_PROC_DISTANCE_NAME registered. The AM calls it with a strategy
 * number through index_getprocinfo and FunctionCall1Coll(Int16GetDatum(
 * strategy)), getting back text: with the strategy of the class's ORDER BY
 * operator (pg_amop, amoppurpose = 'o') for the index DDL, with the scan's
 * order-by strategy for a search.
 *
 * Per-index dims come from the column typmod (pgvector stores the dimension
 * count there); a column with typmod -1 cannot be indexed.
 */
#ifndef CHDB_VECTOR_H
#define CHDB_VECTOR_H

/* ORDER BY operator strategy numbers, one per opclass. */
#define CHDB_VECTOR_STRATEGY_L2 1     /* <->, vector_l2_ops */
#define CHDB_VECTOR_STRATEGY_COSINE 2 /* <=>, vector_cosine_ops */
#define CHDB_VECTOR_STRATEGY_IP 3     /* <#>, vector_ip_ops */

/*
 * Support function numbers: chdb.vector_distance_name(int2) RETURNS text
 * and chdb.vector_query_settings(int2) RETURNS text, both taking a strategy.
 * Number 1 is the access method's options function (amoptsprocnum), which
 * these classes do not have; am.c sizes amsupport to the numbers here, and
 * validate.c checks the signatures.
 */
#define CHDB_VECTOR_PROC_DISTANCE_NAME 2
#define CHDB_VECTOR_PROC_QUERY_SETTINGS 3

/* ClickHouse function per strategy. */
#define CHDB_VECTOR_FN_L2 "L2Distance"
#define CHDB_VECTOR_FN_COSINE "cosineDistance"
#define CHDB_VECTOR_FN_IP "dotProduct"

/* The column's ClickHouse type. */
#define CHDB_VECTOR_CH_TYPE "Array(Float32)"

/*
 * The skip index TYPE; the arguments are the function name from the support
 * function and the typmod dimension count. The AM names the index after the
 * column, quoted as it quotes every identifier:
 *
 *   INDEX "<col>_idx" "<col>" TYPE vector_similarity('hnsw', '<fn>', <dims>)
 */
#define CHDB_VECTOR_INDEX_TYPE "vector_similarity('hnsw', '%s', %d)"

/*
 * A search is one SELECT in the shape ClickHouse's vector search
 * optimization recognizes: the distance function itself as the one sort
 * key, ascending for L2Distance and cosineDistance, descending for
 * dotProduct, over a LIMIT no larger than max_limit_for_vector_search_queries,
 * which the AM reads from the session with getSetting(), as an index scan
 * has no LIMIT of its own. The query vector is a literal `[f, f, ...]`.
 *
 *   SELECT ctid, cosineDistance(col, q) AS _distance FROM t
 *     ORDER BY cosineDistance(col, q)
 *     LIMIT getSetting('max_limit_for_vector_search_queries') SETTINGS ...
 *
 * dotProduct is a similarity, so pgvector's `<#>` (the negative inner
 * product, ascending) sorts it descending, and the distance Postgres gets
 * back is -dotProduct(col, q); `-dotProduct(col, q) ASC` the index cannot
 * use.
 */
#define CHDB_VECTOR_SETTING_MAX_LIMIT "max_limit_for_vector_search_queries"

/*
 * ClickHouse query settings; chdb.vector_query_settings(strategy) returns
 * them as one fragment, e.g.
 *   SETTINGS hnsw_candidate_list_size_for_search = 256,
 *            vector_search_with_rescoring = 0,
 *            vector_search_filter_strategy = 'auto'
 * to be appended after LIMIT. The fragment is never NULL. A dotProduct
 * search rescores whatever the GUC says: without rescoring ClickHouse sorts
 * by the distance the index returns, which for the inner product is
 * usearch's 1 - dot, so the descending sort would come out inverted
 * (ClickHouse 26.9).
 */
#define CHDB_VECTOR_SETTING_CANDIDATES "hnsw_candidate_list_size_for_search"
#define CHDB_VECTOR_SETTING_RESCORING "vector_search_with_rescoring"
#define CHDB_VECTOR_SETTING_FILTER "vector_search_filter_strategy"

/* The GUCs behind them, defined in chdb_vector.so. */
#define CHDB_VECTOR_GUC_CANDIDATES "chdb_vector.hnsw_candidate_list_size"
#define CHDB_VECTOR_GUC_RESCORING "chdb_vector.rescoring"
#define CHDB_VECTOR_GUC_FILTER "chdb_vector.filter_strategy"

#endif
