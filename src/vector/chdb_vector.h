/*
 * The contract between the chdb_vector extension and the chdb access method
 * (and the CustomScan). The AM reads it from the catalogs and calls the
 * support function; it never links against chdb_vector.
 *
 * Each pgvector `vector` column indexed by a chdb opclass becomes:
 *
 *   - a column `<name> Array(Float32)` in the ClickHouse table. The AM
 *     encodes it into Native by casting through `vector::real[]` (pgvector's
 *     implicit-in-assignment cast, vector_to_float4), then writing each
 *     element as Float32.
 *   - a skip index (see CHDB_VECTOR_INDEX_DDL).
 *
 * An opclass is recognized as a vector opclass when it has support function
 * CHDB_VECTOR_PROC_DISTANCE_NAME registered. The AM calls it with the
 * strategy number of the opclass's single ORDER BY operator (found in
 * pg_amop with amoppurpose = 'o') through index_getprocinfo and
 * FunctionCall1Coll(Int16GetDatum(strategy)), getting back text.
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
 * Support function number of chdb.vector_distance_name(int2) RETURNS text.
 * Number 1 is the access method's options function (amoptsprocnum), which
 * these classes do not have; am.c sizes amsupport to the numbers here, and
 * validate.c checks the signature.
 */
#define CHDB_VECTOR_PROC_DISTANCE_NAME 2

/* ClickHouse function per strategy. */
#define CHDB_VECTOR_FN_L2 "L2Distance"
#define CHDB_VECTOR_FN_COSINE "cosineDistance"
#define CHDB_VECTOR_FN_IP "dotProduct"

/*
 * Skip index DDL fragment; the arguments are the column name, the function
 * name from the support function, and the typmod dimension count.
 *
 *   INDEX <col>_idx <col> TYPE vector_similarity('hnsw', '<fn>', <dims>)
 *
 * Example: INDEX embedding_idx embedding TYPE
 *          vector_similarity('hnsw', 'cosineDistance', 1536)
 */
#define CHDB_VECTOR_INDEX_DDL "INDEX %s_idx %s TYPE vector_similarity('hnsw', '%s', %d)"

/*
 * ORDER BY fragment, one per strategy. The query vector is a literal
 * `[f, f, ...]` array of Float32 (cast with CAST(... AS Array(Float32))).
 * The ClickHouse index is used only for `ORDER BY <fn>(col, q) ASC LIMIT k`
 * with the same function the index was built with, so dotProduct, which is
 * a similarity, must be inverted to match pgvector's `<#>` (negative inner
 * product, ascending):
 *
 *   L2     ORDER BY L2Distance(col, q) ASC
 *   cosine ORDER BY cosineDistance(col, q) ASC
 *   ip     ORDER BY dotProduct(col, q) DESC   -- NOT -dotProduct(col, q) ASC,
 *                                                which the index cannot use
 *
 * The distance returned to Postgres for `<#>` is -dotProduct.
 */
#define CHDB_VECTOR_ORDER_ASC "ASC"
#define CHDB_VECTOR_ORDER_DESC "DESC"

/*
 * ClickHouse query settings; chdb.vector_query_settings() returns them
 * as one fragment, e.g.
 *   SETTINGS hnsw_candidate_list_size_for_search = 256,
 *            vector_search_with_rescoring = 0,
 *            vector_search_filter_strategy = 'auto'
 * to be appended after LIMIT. The fragment is empty-safe: it is never NULL.
 */
#define CHDB_VECTOR_SETTING_CANDIDATES "hnsw_candidate_list_size_for_search"
#define CHDB_VECTOR_SETTING_RESCORING "vector_search_with_rescoring"
#define CHDB_VECTOR_SETTING_FILTER "vector_search_filter_strategy"

/* The GUCs behind them, defined in chdb_vector.so. */
#define CHDB_VECTOR_GUC_CANDIDATES "chdb_vector.hnsw_candidate_list_size"
#define CHDB_VECTOR_GUC_RESCORING "chdb_vector.rescoring"
#define CHDB_VECTOR_GUC_FILTER "chdb_vector.filter_strategy"

/*
 * ClickHouse refuses vector search for LIMIT above
 * max_limit_for_vector_search_queries (default 100); the AM or CustomScan
 * must fall back to a non-index plan or raise the setting.
 */

#endif
