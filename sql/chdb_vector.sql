-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION chdb_vector" to load this file. \quit

-- Strategy number in, ClickHouse distance function name out: support
-- function 2 of each class below, by which the access method tells a vector
-- column and the function its skip index is built with (see
-- src/vector/chdb_vector.h; function 1 is the access method's options
-- function). Note that dotProduct is a similarity: its ORDER BY is DESC.
CREATE FUNCTION vector_distance_name(int2) RETURNS text
AS 'MODULE_PATHNAME', 'chdb_vector_distance_name'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- A ClickHouse SETTINGS fragment built from the chdb_vector.* GUCs.
CREATE FUNCTION vector_query_settings() RETURNS text
AS 'MODULE_PATHNAME', 'chdb_vector_query_settings'
LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- One class per distance, each with the one ORDER BY operator of its
-- distance (strategies 1 <->, 2 <=>, 3 <#>, as the support function numbers
-- them). The chdb access method comes from chdb_search, which the control
-- file requires, as it does pgvector for the type and the operators.
CREATE OPERATOR CLASS vector_l2_ops FOR TYPE vector USING chdb AS
    OPERATOR 1 <-> (vector, vector) FOR ORDER BY pg_catalog.float_ops,
    FUNCTION 2 chdb.vector_distance_name(int2);

CREATE OPERATOR CLASS vector_cosine_ops FOR TYPE vector USING chdb AS
    OPERATOR 2 <=> (vector, vector) FOR ORDER BY pg_catalog.float_ops,
    FUNCTION 2 chdb.vector_distance_name(int2);

CREATE OPERATOR CLASS vector_ip_ops FOR TYPE vector USING chdb AS
    OPERATOR 3 <#> (vector, vector) FOR ORDER BY pg_catalog.float_ops,
    FUNCTION 2 chdb.vector_distance_name(int2);
