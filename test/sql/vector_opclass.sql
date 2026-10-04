-- chdb_vector's operator classes through the chdb index access method, end to
-- end: a vector column is an Array(Float32) column with an HNSW skip index in
-- the index's ClickHouse table, and an ORDER BY on one of its distance
-- operators is answered by the database's worker, nearest first. The stub
-- client returns the ctids its GUC names, so this runs with the worker only,
-- like search_e2e; vector_support covers the functions with either client.
\set VERBOSITY terse
CREATE EXTENSION vector;
CREATE EXTENSION chdb_search;
CREATE EXTENSION chdb_vector;
SET search_path = public, chdb;
SET chdb_search.mask_oids = on;

-- The classes: one ORDER BY operator each, and the two support functions the
-- access method tells them by (function 1 is its options function).
SELECT opcname, amvalidate(c.oid), amopstrategy, amoppurpose, amopopr::regoperator
  FROM pg_opclass c JOIN pg_am a ON a.oid = c.opcmethod
  JOIN pg_amop o ON o.amopfamily = c.opcfamily
 WHERE a.amname = 'chdb' AND opcname LIKE 'vector\_%' ORDER BY 1;
SELECT amprocnum, amproc::regprocedure
  FROM pg_amproc p JOIN pg_opclass c ON c.opcfamily = p.amprocfamily
 WHERE opcname = 'vector_cosine_ops' AND c.opcmethod = (SELECT oid FROM pg_am WHERE amname = 'chdb')
 ORDER BY 1;

CREATE TABLE items (id int PRIMARY KEY, body text, embedding vector(3));
INSERT INTO items VALUES
    (1, 'running shoes', '[1,0,0]'),
    (2, 'sandals', '[0,1,0]'),
    (3, 'running socks', '[0.9,0.1,0]'),
    (4, 'boots', '[0,0,1]');
-- The table: Array(Float32) under a vector_similarity index built with the
-- class's function and the column's dimension.
SET client_min_messages = debug1;
CREATE INDEX items_idx ON items USING chdb (body, embedding vector_cosine_ops);
RESET client_min_messages;
-- A column without a dimension cannot be indexed, and a NULL vector cannot
-- be stored: ClickHouse wants every array at the index's dimension.
CREATE TABLE nodims (embedding vector);
CREATE INDEX ON nodims USING chdb (embedding vector_cosine_ops);
DROP TABLE nodims;
INSERT INTO items VALUES (5, 'no embedding', NULL);

----------------------------------------------------------------------------
-- Nearest first through the index
----------------------------------------------------------------------------
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM items ORDER BY embedding <=> '[1,0,0]' LIMIT 3;
-- The statement: the distance as _distance, the function itself as the sort
-- key, as many rows as the index serves, and chdb_vector's settings.
SET client_min_messages = debug1;
SELECT id, embedding <=> '[1,0,0]' AS distance FROM items ORDER BY embedding <=> '[1,0,0]' LIMIT 3;
RESET client_min_messages;
-- The same rows by sequential scan.
SET enable_seqscan = on;
SET enable_indexscan = off;
SELECT id, embedding <=> '[1,0,0]' AS distance FROM items ORDER BY embedding <=> '[1,0,0]' LIMIT 3;
RESET enable_indexscan;
SET enable_seqscan = off;
-- A hybrid query: the text predicate filters in ClickHouse, the distance
-- orders what passes.
EXPLAIN (COSTS OFF) SELECT id FROM items WHERE body @@@ 'running' ORDER BY embedding <=> '[0,1,0]' LIMIT 2;
SET client_min_messages = debug1;
SELECT id FROM items WHERE body @@@ 'running' ORDER BY embedding <=> '[0,1,0]' LIMIT 2;
RESET client_min_messages;
-- The settings follow the GUCs.
SET chdb_vector.hnsw_candidate_list_size = 64;
SET chdb_vector.filter_strategy = prefilter;
SET client_min_messages = debug1;
SELECT id FROM items WHERE body @@@ 'running' ORDER BY embedding <=> '[0,1,0]' LIMIT 1;
RESET client_min_messages;
RESET chdb_vector.hnsw_candidate_list_size;
RESET chdb_vector.filter_strategy;
-- Several order-bys, or a NULL argument from a generic plan, are no search
-- the index serves: they sort every row, as in search_am_scan.
SET client_min_messages = debug1;
SELECT id FROM items ORDER BY embedding <=> '[0,0,1]', embedding <=> '[1,0,0]' LIMIT 2;
SET plan_cache_mode = force_generic_plan;
PREPARE near(vector) AS SELECT id FROM items ORDER BY embedding <=> $1 LIMIT 2;
EXECUTE near(NULL);
EXECUTE near('[0,0,1]');
RESET plan_cache_mode;
RESET client_min_messages;

----------------------------------------------------------------------------
-- dotProduct is a similarity: sorted descending, rescored, negated for <#>
----------------------------------------------------------------------------
CREATE INDEX items_ip ON items USING chdb (embedding vector_ip_ops);
SET client_min_messages = debug1;
SELECT id, embedding <#> '[0.9,0.1,0]' AS distance FROM items ORDER BY embedding <#> '[0.9,0.1,0]' LIMIT 3;
RESET client_min_messages;
DROP INDEX items_ip;
CREATE INDEX items_l2 ON items USING chdb (embedding vector_l2_ops);
SET client_min_messages = debug1;
SELECT id, embedding <-> '[0,0,1]' AS distance FROM items ORDER BY embedding <-> '[0,0,1]' LIMIT 2;
RESET client_min_messages;
DROP INDEX items_l2;

----------------------------------------------------------------------------
-- The scan asks for as many rows as max_limit_for_vector_search_queries
-- lets the index return, and gets no more
----------------------------------------------------------------------------
SELECT chdb_search_exec('SET max_limit_for_vector_search_queries = 2');
SELECT id FROM items ORDER BY embedding <=> '[1,0,0]' LIMIT 3;
SET enable_seqscan = on;
SET enable_indexscan = off;
SELECT id FROM items ORDER BY embedding <=> '[1,0,0]' LIMIT 3;
RESET enable_indexscan;
SET enable_seqscan = off;

DROP TABLE items;
DROP EXTENSION chdb_vector;
DROP EXTENSION chdb_search;
DROP EXTENSION vector;

-- The worker stays connected to the database, which could not be dropped for
-- the next run, so stop it; the setting above goes with its engine.
DO $$
BEGIN
    PERFORM pg_terminate_backend(pid, 10000)
       FROM pg_stat_activity
      WHERE backend_type = 'chdb_search worker' AND datname = current_database();
END $$;
