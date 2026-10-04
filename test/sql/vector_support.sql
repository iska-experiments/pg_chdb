-- chdb_vector: support functions, settings, and the vector cast.
CREATE EXTENSION vector;
CREATE EXTENSION chdb_search;
CREATE EXTENSION chdb_vector;

SELECT n, chdb.vector_distance_name(n::int2) FROM generate_series(1, 3) AS n;
SELECT chdb.vector_distance_name(4::int2);

-- The settings of a search by strategy 2 (cosine); a dotProduct search (3)
-- always rescores, see src/vector/chdb_vector.h.
SELECT chdb.vector_query_settings(2::int2);
SELECT chdb.vector_query_settings(3::int2);
SET chdb_vector.hnsw_candidate_list_size = 64;
SET chdb_vector.rescoring = on;
SET chdb_vector.filter_strategy = prefilter;
SELECT chdb.vector_query_settings(2::int2);
SET chdb_vector.filter_strategy = bogus;
RESET ALL;

SELECT '[1,2,3]'::vector::real[];
