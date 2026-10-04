-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION chdb_vector" to load this file. \quit

-- Strategy number in, ClickHouse distance function name out. Registered as
-- support function 1 of each opclass; see src/vector/chdb_vector.h. Note that
-- dotProduct is a similarity: its ORDER BY is DESC.
CREATE FUNCTION vector_distance_name(int2) RETURNS text
AS 'MODULE_PATHNAME', 'chdb_vector_distance_name'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- A ClickHouse SETTINGS fragment built from the chdb_vector.* GUCs.
CREATE FUNCTION vector_query_settings() RETURNS text
AS 'MODULE_PATHNAME', 'chdb_vector_query_settings'
LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- requires the chdb access method from chdb_search
DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_am WHERE amname = 'chdb') THEN
        RAISE NOTICE 'access method chdb not found; operator classes not created';
        RETURN;
    END IF;

    EXECUTE $c$
        CREATE OPERATOR CLASS vector_l2_ops FOR TYPE vector USING chdb AS
            OPERATOR 1 <-> (vector, vector) FOR ORDER BY pg_catalog.float_ops,
            FUNCTION 1 chdb.vector_distance_name(int2)
    $c$;
    EXECUTE $c$
        CREATE OPERATOR CLASS vector_cosine_ops FOR TYPE vector USING chdb AS
            OPERATOR 2 <=> (vector, vector) FOR ORDER BY pg_catalog.float_ops,
            FUNCTION 1 chdb.vector_distance_name(int2)
    $c$;
    EXECUTE $c$
        CREATE OPERATOR CLASS vector_ip_ops FOR TYPE vector USING chdb AS
            OPERATOR 3 <#> (vector, vector) FOR ORDER BY pg_catalog.float_ops,
            FUNCTION 1 chdb.vector_distance_name(int2)
    $c$;
END
$$;
