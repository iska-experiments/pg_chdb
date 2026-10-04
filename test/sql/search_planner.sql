-- The chdb_search custom scan, end to end: a search over a table with a chdb
-- index is planned as one ClickHouse statement, which the database's worker
-- answers from the index's table, and the rows come from the heap by ctid.
-- The server needs libchdb on its library path, for chdb_search_engine;
-- search_stub_planner covers the plans with the stub client.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;
SET chdb_search.mask_oids = on;

CREATE TABLE docs (
    id int PRIMARY KEY, body text, tags text[], author text COLLATE "C",
    price numeric(10, 2), loc point
);
INSERT INTO docs VALUES
    (1, 'Running shoes for runners', '{Sport,shoes}', 'ann', 49.90, '(0,0)'),
    (2, 'Walking boots', '{outdoor}', 'bob', 89.00, '(10,10)'),
    (3, 'Trail running shoes, fast and light', '{sport,trail}', 'ann', 120.00, '(1,1)'),
    (4, 'Running socks', '{sport}', 'cid', 5.00, '(3,4)'),
    (5, NULL, NULL, NULL, NULL, NULL);
-- Postgres's point distance stands in for chdb_vector's operators, which
-- vector_planner covers.
ALTER OPERATOR FAMILY columnar_ops USING chdb ADD OPERATOR 1 <-> (point, point) FOR ORDER BY float_ops;
CREATE INDEX docs_idx ON docs USING chdb (body, tags text_array_ops, author columnar_ops, price, loc);
SET enable_seqscan = off;
SET enable_bitmapscan = off;

-- Runs a query through the custom scan and by sequential scan: whether the
-- plan was a custom scan, and the rows each returned that the other did not.
CREATE FUNCTION pg_temp.check(q text, OUT custom_scan bool, OUT only_custom bigint, OUT only_seqscan bigint)
LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
    custom_scan := false;
    SET LOCAL enable_seqscan = off;
    SET LOCAL enable_indexscan = off;
    FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
        custom_scan := custom_scan OR line LIKE '%Custom Scan (chdb_search)%';
    END LOOP;
    EXECUTE format('CREATE TEMP TABLE custom_rows ON COMMIT DROP AS %s', q);
    SET LOCAL enable_seqscan = on;
    SET LOCAL chdb_search.enable_custom_scan = off;
    EXECUTE format('CREATE TEMP TABLE seqscan_rows ON COMMIT DROP AS %s', q);
    SELECT count(*) INTO only_custom FROM (TABLE custom_rows EXCEPT ALL TABLE seqscan_rows) d;
    SELECT count(*) INTO only_seqscan FROM (TABLE seqscan_rows EXCEPT ALL TABLE custom_rows) d;
END $$;
-- EXPLAIN without costs, and with actual the rows of EXPLAIN ANALYZE, as
-- Postgres 18 prints them: 17 prints a row count without decimals, and 19
-- names an InitPlan by what it computes.
CREATE FUNCTION pg_temp.explain(q text, actual bool DEFAULT false) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
    FOR line IN EXECUTE 'EXPLAIN (COSTS OFF' || CASE WHEN actual
        THEN ', ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF' ELSE '' END || ') ' || q LOOP
        line := regexp_replace(line, 'rows=(\d+) loops', 'rows=\1.00 loops', 'g');
        RETURN NEXT replace(line, 'InitPlan expr_', 'InitPlan ');
    END LOOP;
END $$;

----------------------------------------------------------------------------
-- Plans: a search predicate, as an operator or a function, makes a custom
-- scan that sends it with the filters the index can take, and keeps the
-- rest as its own filter
----------------------------------------------------------------------------
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'running shoes';
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE chdb.has_any_tokens(body, 'boots socks') AND tags @@= 'sport';
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@? 'boots socks' AND author = 'bob' AND 10 < price;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'running' AND id > 1 AND author <> 'bob';
EXPLAIN (VERBOSE, COSTS OFF) SELECT id, body FROM docs WHERE chdb.has_token(body, 'boots') AND length(body) > 5;
-- A filter alone is the index's; an order alone is the store's, with the
-- LIMIT when every clause went along, and without it when one stayed here.
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE price < 50;
EXPLAIN (COSTS OFF) SELECT id FROM docs ORDER BY loc <-> '(0,0)' LIMIT 2;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'running' ORDER BY loc <-> '(0,0)' OFFSET 1 LIMIT 2;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'running' AND id > 1 ORDER BY loc <-> '(0,0)' LIMIT 2;
-- No LIMIT goes along with a sort the store does not do all of, or with
-- grouping, DISTINCT or row locks between the scan and the LIMIT.
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'running' ORDER BY loc <-> '(0,0)', id LIMIT 2;
EXPLAIN (COSTS OFF) SELECT DISTINCT ON (loc <-> '(0,0)') id FROM docs WHERE body @@@ 'running' ORDER BY loc <-> '(0,0)' LIMIT 2;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'running' ORDER BY loc <-> '(0,0)' LIMIT 2 FOR UPDATE;
-- The statement shows masked, as the log is; unmasked it names the table.
SET chdb_search.mask_oids = off;
CREATE FUNCTION pg_temp.plan(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
BEGIN RETURN QUERY EXECUTE 'EXPLAIN (COSTS OFF) ' || q; END $$;
SELECT line ~ 'ClickHouse: SELECT ctid FROM idx_\d+\.t_\d+ WHERE' AS names_the_table
  FROM pg_temp.plan($$SELECT id FROM docs WHERE body @@@ 'running shoes'$$) line
 WHERE line LIKE '%ClickHouse:%';
SET chdb_search.mask_oids = on;

----------------------------------------------------------------------------
-- The rows are the sequential scan's
----------------------------------------------------------------------------
SELECT * FROM pg_temp.check($$SELECT id FROM docs WHERE body @@@ 'running shoes'$$);
SELECT * FROM pg_temp.check($$SELECT id FROM docs WHERE chdb.has_any_tokens(body, 'boots socks') AND tags @@= 'sport'$$);
SELECT * FROM pg_temp.check($$SELECT id FROM docs WHERE body @@? 'boots socks' AND author = 'bob' AND 10 < price$$);
SELECT * FROM pg_temp.check($$SELECT id FROM docs WHERE body @@@ 'running' AND id > 1 AND author <> 'bob'$$);
SELECT * FROM pg_temp.check($$SELECT id, body FROM docs WHERE chdb.has_token(body, 'boots') AND length(body) > 5$$);
SELECT * FROM pg_temp.check($$SELECT id FROM docs WHERE body @@~ 'running shoes' AND price >= 49.90 AND price <= 120$$);
SELECT * FROM pg_temp.check($$SELECT id FROM docs WHERE tags @@@ 'SPORT' AND body @@= 'running'$$);
SELECT * FROM pg_temp.check($$SELECT id FROM docs ORDER BY loc <-> '(0,0)' LIMIT 2$$);
SELECT * FROM pg_temp.check($$SELECT id FROM docs WHERE body @@@ 'running' ORDER BY loc <-> '(0,0)' OFFSET 1 LIMIT 2$$);
SELECT * FROM pg_temp.check($$SELECT id FROM docs WHERE body @@@ 'running' AND id > 1 ORDER BY loc <-> '(0,0)' LIMIT 2$$);
SELECT * FROM pg_temp.check($$SELECT id FROM docs WHERE body @@@ 'nothing here'$$);
-- The query language goes along as well: a chdb.query tree, which carries
-- the OR and NOT the scan keys cannot, a regex and a wildcard, as operators
-- and as functions, all members of the column's operator family.
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ (chdb.match_all('running shoes') && !chdb.term('trail')) AND price < 100;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@/ '^run' AND chdb.wildcard(body, '%shoes%');
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE chdb.query_matches(tags, chdb.term('sport') && !chdb.term('trail'));
SELECT * FROM pg_temp.check($$SELECT id FROM docs WHERE body @@@ (chdb.match_all('running shoes') && !chdb.term('trail')) AND price < 100$$);
SELECT * FROM pg_temp.check($$SELECT id FROM docs WHERE body @@/ '^run' AND chdb.wildcard(body, '%shoes%')$$);
SELECT * FROM pg_temp.check($$SELECT id FROM docs WHERE chdb.regex(body, 'boot') AND body @@% 'walking%'$$);
SELECT * FROM pg_temp.check($$SELECT id FROM docs WHERE body @@@ (chdb.phrase('running shoes', 1) || chdb.regex('socks$'))$$);
SELECT * FROM pg_temp.check($$SELECT id FROM docs WHERE chdb.query_matches(tags, chdb.term('sport') && !chdb.term('trail'))$$);
-- The order is the store's, nearest first.
SELECT id, loc <-> '(0,0)' AS distance FROM docs WHERE body @@@ 'running' ORDER BY loc <-> '(0,0)' LIMIT 2;
-- A NULL needle: folded away when written, no statement from a generic plan.
PREPARE q(text, int) AS SELECT id FROM docs WHERE body @@@ $1 AND id > $2;
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE q(NULL, 0);
EXECUTE q(NULL, 0);
EXECUTE q('running', 1);
RESET plan_cache_mode;
DEALLOCATE q;
-- An argument a subplan computes is there at execution only.
SELECT * FROM pg_temp.explain($$SELECT id FROM docs WHERE body @@@ (SELECT 'boots'::text)$$);
SELECT id FROM docs WHERE body @@@ (SELECT 'boots'::text);
-- A rescan with another needle sends another statement; plain EXPLAIN has
-- no value for a parameter a subplan computes, ANALYZE shows the last one.
EXPLAIN (COSTS OFF)
SELECT q, d.id FROM (VALUES ('boots'), ('running')) v(q), LATERAL (SELECT id FROM docs WHERE body @@@ v.q OFFSET 0) d;
SELECT * FROM pg_temp.explain($$SELECT q, d.id FROM (VALUES ('boots'), ('running')) v(q), LATERAL (SELECT id FROM docs WHERE body @@@ v.q OFFSET 0) d$$, true);
SELECT q, d.id FROM (VALUES ('boots'), ('running')) v(q), LATERAL (SELECT id FROM docs WHERE body @@@ v.q OFFSET 0) d ORDER BY 1, 2;
-- Row locks recheck the pushed clauses in Postgres.
SELECT id FROM docs WHERE body @@@ 'running' AND price > 10 ORDER BY id FOR UPDATE;

----------------------------------------------------------------------------
-- Visibility is the heap's: rows the store still holds for deleted or
-- updated tuples are not returned, and a pushed LIMIT is still met
----------------------------------------------------------------------------
SELECT chdb_search_store_table('docs_idx') AS tbl \gset
DELETE FROM docs WHERE id = 1;
UPDATE docs SET body = 'Trail running sandals' WHERE id = 3;
SELECT * FROM chdb_search_query(format('SELECT count() FROM %s WHERE hasAllTokens(body, ''shoes'')', :'tbl')) AS (store_rows bigint);
SELECT id, body FROM docs WHERE body @@@ 'running shoes';
SELECT * FROM pg_temp.check($$SELECT id FROM docs WHERE body @@@ 'running'$$);
-- The nearest row is the deleted one: the scan asks for more.
SELECT * FROM pg_temp.explain($$SELECT id FROM docs ORDER BY loc <-> '(0,0)' LIMIT 2$$, true);
SELECT * FROM pg_temp.check($$SELECT id FROM docs ORDER BY loc <-> '(0,0)' LIMIT 2$$);
SELECT * FROM pg_temp.check($$SELECT id FROM docs ORDER BY loc <-> '(0,0)' LIMIT 1$$);
-- A transaction sees its own rows, as through the index: the statement
-- reads the transaction's staging table with the index's table.
BEGIN;
INSERT INTO docs VALUES (6, 'Running late', '{}', 'dee', 1.00, '(0,1)');
SELECT id FROM docs WHERE body @@@ 'running late';
COMMIT;
SELECT id FROM docs WHERE body @@@ 'running late';

----------------------------------------------------------------------------
-- The settings: off leaves the index scan, a factor above 1 prefers it
----------------------------------------------------------------------------
SET chdb_search.enable_custom_scan = off;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'running shoes';
RESET chdb_search.enable_custom_scan;
SET chdb_search.custom_scan_cost_factor = 10;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'running shoes';
RESET chdb_search.custom_scan_cost_factor;

DROP TABLE docs;
ALTER OPERATOR FAMILY columnar_ops USING chdb DROP OPERATOR 1 (point, point);
DROP EXTENSION chdb_search;

-- The worker stays connected to the database, which could not be dropped for
-- the next run, so stop it.
DO $$
BEGIN
    PERFORM pg_terminate_backend(pid, 10000)
       FROM pg_stat_activity
      WHERE backend_type = 'chdb_search worker' AND datname = current_database();
END $$;
