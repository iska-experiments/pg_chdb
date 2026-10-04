-- The chdb_search aggregate scan, end to end: a GROUP BY or aggregate query
-- over a table with a chdb index, every clause of which the store applies,
-- is planned as one ClickHouse statement that computes the groups and the
-- aggregates, when the heap's visibility map says every page is all-visible;
-- when the heap cannot vouch for the store at execution, the node runs the
-- plan Postgres would have run instead, so the answer is always the heap's.
-- The server needs libchdb on its library path, for chdb_search_engine;
-- search_stub_aggregates covers the plans with the stub client.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;
SET chdb_search.mask_oids = on;

CREATE TABLE docs (
    id int PRIMARY KEY, body text, tags text[], author text COLLATE "C",
    price numeric(10, 2), qty int, weight float4, ratio float8, big int8,
    sold date, seen timestamptz, ok bool
);
INSERT INTO docs VALUES
    (1, 'Running shoes for runners', '{Sport,shoes}', 'ann', 49.90, 3, 1.5, 0.25,
     10000000000, '2026-01-01', '2026-01-01 10:00+00', true),
    (2, 'Walking boots', '{outdoor}', 'bob', 89.00, 1, 2.25, 0.5,
     20000000000, '2026-02-01', '2026-02-01 10:00+00', false),
    (3, 'Trail running shoes, fast and light', '{sport,trail}', 'ann', 120.00, 2, 0.75, 1,
     30000000000, '2026-03-01', '2026-03-01 10:00+00', true),
    (4, 'Running socks', '{sport}', 'cid', 5.00, 7, 0.125, 1.5,
     40000000000, NULL, NULL, NULL),
    (5, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
CREATE INDEX docs_idx ON docs USING chdb (
    body, tags text_array_ops, author columnar_ops, price, qty, weight, ratio, big,
    sold, seen, ok
);
-- Sets every page all-visible, which the aggregate scan asks of the heap.
VACUUM docs;
SET enable_seqscan = off;

-- Runs a query through the aggregate scan and as Postgres computes it:
-- whether the plan was an aggregate scan, and the rows each returned that
-- the other did not.
CREATE FUNCTION pg_temp.check(q text, OUT aggregate_scan bool, OUT only_pushed bigint, OUT only_plain bigint)
LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
    aggregate_scan := false;
    FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
        aggregate_scan := aggregate_scan OR line LIKE '%Custom Scan (chdb_search aggregate)%';
    END LOOP;
    EXECUTE format('CREATE TEMP TABLE pushed_rows ON COMMIT DROP AS %s', q);
    SET LOCAL enable_seqscan = on;
    SET LOCAL chdb_search.enable_aggregate_pushdown = off;
    EXECUTE format('CREATE TEMP TABLE plain_rows ON COMMIT DROP AS %s', q);
    SELECT count(*) INTO only_pushed FROM (TABLE pushed_rows EXCEPT ALL TABLE plain_rows) d;
    SELECT count(*) INTO only_plain FROM (TABLE plain_rows EXCEPT ALL TABLE pushed_rows) d;
END $$;

----------------------------------------------------------------------------
-- Plans: the groups and the aggregates in one statement, the HAVING clause
-- and the rest of the target list computed here, and the plan Postgres
-- would have run carried as the child that answers when the store cannot
----------------------------------------------------------------------------
EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE body @@@ 'running';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT author, count(*), count(price), min(price), max(qty), sum(qty), avg(qty)
  FROM docs WHERE body @@? 'running boots' AND price < 100 GROUP BY author;
-- A sum is typed as Postgres types it, avg is the store's sum and count.
EXPLAIN (COSTS OFF)
SELECT sum(weight), avg(weight), sum(ratio), sum(big), avg(big), sum(price), avg(price) FROM docs;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT upper(author), count(*) + 1 FROM docs WHERE price < 100 GROUP BY author HAVING sum(qty) > 1;
EXPLAIN (COSTS OFF) SELECT ok, min(sold), max(seen) FROM docs GROUP BY ok;
-- Not pushed: a clause the store cannot apply, an aggregate it does not
-- compute, a grouping by an expression or by a column outside the index,
-- and count of a text[] column, whose NULL the store cannot tell from empty.
EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE body @@@ 'running' AND id > 1;
EXPLAIN (COSTS OFF) SELECT count(DISTINCT author) FROM docs WHERE body @@@ 'running';
EXPLAIN (COSTS OFF) SELECT string_agg(author, ',') FROM docs WHERE body @@@ 'running';
EXPLAIN (COSTS OFF) SELECT lower(author), count(*) FROM docs GROUP BY lower(author);
EXPLAIN (COSTS OFF) SELECT id, count(*) FROM docs WHERE body @@@ 'running' GROUP BY id;
EXPLAIN (COSTS OFF) SELECT count(tags) FROM docs;
-- The settings: either off leaves the aggregate to Postgres.
SET chdb_search.enable_aggregate_pushdown = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE body @@@ 'running';
RESET chdb_search.enable_aggregate_pushdown;
SET chdb_search.enable_custom_scan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE body @@@ 'running';
RESET chdb_search.enable_custom_scan;

----------------------------------------------------------------------------
-- The answers are Postgres's
----------------------------------------------------------------------------
SELECT * FROM pg_temp.check($$SELECT count(*) FROM docs WHERE body @@@ 'running'$$);
SELECT * FROM pg_temp.check($$SELECT count(*) AS n, count(body) AS b, count(price) AS p, count(sold) AS s FROM docs$$);
SELECT * FROM pg_temp.check($$SELECT min(price) AS a, max(price) AS b, min(qty) AS c, max(qty) AS d, min(author) AS e, max(author) AS f FROM docs$$);
SELECT * FROM pg_temp.check($$SELECT min(sold) AS a, max(sold) AS b, min(seen) AS c, max(seen) AS d, min(ratio) AS e, max(weight) AS f FROM docs$$);
SELECT * FROM pg_temp.check($$SELECT sum(qty) AS a, sum(big) AS b, sum(weight) AS c, sum(ratio) AS d, sum(price) AS e FROM docs$$);
SELECT * FROM pg_temp.check($$SELECT avg(qty) AS a, avg(big) AS b, avg(weight) AS c, avg(ratio) AS d, avg(price) AS e FROM docs$$);
SELECT * FROM pg_temp.check($$SELECT author, count(*), sum(qty), avg(price) FROM docs GROUP BY author$$);
SELECT * FROM pg_temp.check($$SELECT ok, sold, count(*) FROM docs GROUP BY ok, sold$$);
SELECT * FROM pg_temp.check($$SELECT body, count(*) FROM docs WHERE body @@@ 'running' GROUP BY body$$);
SELECT * FROM pg_temp.check($$SELECT author FROM docs WHERE body @@@ 'shoes' GROUP BY author$$);
SELECT * FROM pg_temp.check($$SELECT upper(author), count(*) + 1 FROM docs WHERE price < 100 GROUP BY author HAVING sum(qty) > 1$$);
SELECT * FROM pg_temp.check($$SELECT count(*), sum(qty), avg(qty), min(sold) FROM docs WHERE body @@@ 'nothing here'$$);
SELECT * FROM pg_temp.check($$SELECT author, count(*) FROM docs WHERE body @@@ 'nothing here' GROUP BY author$$);
-- Groups with a NULL key, and no rows: one row for a plain aggregate.
SELECT ok, count(*), sum(qty) FROM docs GROUP BY ok ORDER BY ok;
SELECT count(*), sum(qty), avg(qty), min(sold) FROM docs WHERE body @@@ 'nothing here';
-- avg divides as Postgres does, so the digits are the same.
SELECT avg(qty), avg(big), avg(price), avg(weight) FROM docs WHERE body @@@ 'running';
SET chdb_search.enable_aggregate_pushdown = off;
SELECT avg(qty), avg(big), avg(price), avg(weight) FROM docs WHERE body @@@ 'running';
RESET chdb_search.enable_aggregate_pushdown;
-- A NULL needle from a generic plan matches nothing; a subplan's argument is
-- there at execution only, and a rescan sends another statement.
PREPARE q(text) AS SELECT count(*) FROM docs WHERE body @@@ $1;
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE q(NULL);
EXECUTE q(NULL);
EXECUTE q('running');
RESET plan_cache_mode;
DEALLOCATE q;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT q, c FROM (VALUES ('boots'), ('running')) v(q), LATERAL (SELECT count(*) AS c FROM docs WHERE body @@@ v.q) d;
SELECT q, c FROM (VALUES ('boots'), ('running')) v(q), LATERAL (SELECT count(*) AS c FROM docs WHERE body @@@ v.q) d ORDER BY 1;

----------------------------------------------------------------------------
-- MVCC: the store answers only for a heap whose every page is all-visible,
-- checked at planning and again around the statement; otherwise the plan
-- Postgres would have run answers, inside the node
----------------------------------------------------------------------------
PREPARE c AS SELECT count(*) FROM docs WHERE body @@@ 'running';
SET plan_cache_mode = force_generic_plan;
EXECUTE c;
-- An insert clears its page's bit: a new plan leaves the aggregate to
-- Postgres, the generic plan takes the exact plan at execution.
INSERT INTO docs VALUES (6, 'Running late', '{}', 'dee', 1.00, 1, 1, 1, 1, '2026-04-01', '2026-04-01 10:00+00', true);
EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE body @@@ 'running';
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) EXECUTE c;
EXECUTE c;
SELECT * FROM pg_temp.check($$SELECT author, count(*) FROM docs WHERE body @@@ 'running' GROUP BY author$$);
-- VACUUM sets the bit again, and the store answers.
VACUUM docs;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) EXECUTE c;
EXECUTE c;
-- A deleted row the store still holds: the exact plan does not count it,
-- and once VACUUM has removed it from both the store does not either.
DELETE FROM docs WHERE id = 6;
SELECT chdb_search_store_table('docs_idx') AS tbl \gset
SELECT * FROM chdb_search_query(format('SELECT count() FROM %s WHERE hasAllTokens(body, ''running'')', :'tbl')) AS (store_rows bigint);
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) EXECUTE c;
EXECUTE c;
SELECT * FROM pg_temp.check($$SELECT count(*), sum(qty) FROM docs WHERE body @@@ 'running'$$);
VACUUM docs;
SELECT * FROM chdb_search_query(format('SELECT count() FROM %s WHERE hasAllTokens(body, ''running'')', :'tbl')) AS (store_rows bigint);
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) EXECUTE c;
EXECUTE c;
-- A transaction's own insert takes the exact plan too, whose scan reads
-- the transaction's staged rows with the store's: the row counts at once.
BEGIN;
INSERT INTO docs VALUES (7, 'Running uncommitted', '{}', 'eve', 1.00, 1, 1, 1, 1, '2026-04-01', '2026-04-01 10:00+00', true);
EXECUTE c;
COMMIT;
EXECUTE c;
-- COPY FREEZE into a table truncated in the transaction leaves its pages
-- all-visible, so the store answers, with the rows the transaction staged.
BEGIN;
TRUNCATE docs;
COPY docs (id, body, author) FROM stdin (FREEZE);
8	Running free	fay
9	Running fast	gus
10	Walking slow	hal
\.
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) EXECUTE c;
EXECUTE c;
SELECT * FROM pg_temp.check($$SELECT author, count(*) FROM docs WHERE body @@@ 'running' GROUP BY author$$);
ROLLBACK;
RESET plan_cache_mode;
DEALLOCATE c;

DROP TABLE docs;
DROP EXTENSION chdb_search;

-- The worker stays connected to the database, which could not be dropped for
-- the next run, so stop it.
DO $$
BEGIN
    PERFORM pg_terminate_backend(pid, 10000)
       FROM pg_stat_activity
      WHERE backend_type = 'chdb_search worker' AND datname = current_database();
END $$;
