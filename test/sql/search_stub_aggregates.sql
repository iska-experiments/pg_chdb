-- The chdb_search aggregate scan with the stub worker client (make
-- CHDB_SEARCH_STUB=1), whose store holds the rows chdb_search_stub.ctids
-- names and answers count() with their number: the plans, the answer from
-- the store on an all-visible heap, and the exact plan answering when the
-- heap is not, seen without a worker. Runs only with the stub (see the
-- Makefile); search_aggregates covers the worker.
\set VERBOSITY terse
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;
SET chdb_search.mask_oids = on;

CREATE TABLE docs (id int PRIMARY KEY, body text, author text COLLATE "C", price numeric(10, 2));
INSERT INTO docs VALUES
    (1, 'Running shoes', 'ann', 49.90), (2, 'Walking boots', 'bob', 89.00),
    (3, 'Trail shoes', 'ann', 120.00);
CREATE INDEX docs_idx ON docs USING chdb (body, author columnar_ops, price);
VACUUM docs;
SET enable_seqscan = off;
-- As search_planner's: EXPLAIN as Postgres 18 prints it.
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
-- Plans
----------------------------------------------------------------------------
EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE body @@@ 'running shoes';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT author, count(*), count(price), min(price), max(price), sum(price), avg(price)
  FROM docs WHERE chdb.has_all_tokens(body, 'shoes') AND price < 100 GROUP BY author HAVING count(*) > 1;
EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE body @@@ 'shoes' AND id > 1;
SET chdb_search.enable_aggregate_pushdown = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE body @@@ 'running shoes';
RESET chdb_search.enable_aggregate_pushdown;

----------------------------------------------------------------------------
-- The store's count on an all-visible heap, the exact plan's otherwise
----------------------------------------------------------------------------
-- The stub's store names a row past the heap's end, which the store counts
-- and a scan skips, so the two answers differ.
SET chdb_search_stub.ctids = '1,2,65537';
SELECT * FROM pg_temp.explain($$SELECT count(*) FROM docs WHERE body @@@ 'x'$$, true);
SELECT count(*) FROM docs WHERE body @@@ 'x';
-- An insert clears its page's bit: a new plan leaves the aggregate to
-- Postgres, and the plan made before takes the exact plan at execution,
-- which fetches the store's ctids from the heap, until VACUUM sets the bit
-- again.
PREPARE c AS SELECT count(*) FROM docs WHERE body @@@ 'x';
SET plan_cache_mode = force_generic_plan;
EXECUTE c;
INSERT INTO docs VALUES (4, 'Running socks', 'cid', 5.00);
SELECT * FROM pg_temp.explain($$EXECUTE c$$, true);
EXECUTE c;
EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE body @@@ 'x';
VACUUM docs;
SELECT * FROM pg_temp.explain($$EXECUTE c$$, true);
EXECUTE c;
RESET plan_cache_mode;
DEALLOCATE c;
RESET chdb_search_stub.ctids;
-- No rows at all, as the stub answers by default: none to count.
SELECT count(*) FROM docs WHERE body @@@ 'x';

----------------------------------------------------------------------------
-- The fail-safe check runs before the statement: a store that does not
-- match the metapage is refused, or in skip mode left alone, at planning,
-- where no path is offered, or at execution by a plan made before, where
-- the exact plan answers
----------------------------------------------------------------------------
-- The scans above proved the store for this state of the index; altering
-- the index drops the verdict, so that the store is asked again.
ALTER INDEX docs_idx SET (vacuum_optimize_ratio = 0.5);
SET chdb_search_stub.meta = 'none';
SELECT count(*) FROM docs WHERE body @@@ 'x';
SET chdb_search.unavailable_index = skip;
SET enable_seqscan = on;
EXPLAIN (COSTS OFF) SELECT count(*) FROM docs WHERE body @@@ 'x';
SET enable_seqscan = off;
RESET chdb_search.unavailable_index;
RESET chdb_search_stub.meta;
-- In error mode the planner does not ask the store, so the plan holds the
-- aggregate scan whatever the store says; the scan asks when it runs.
PREPARE c AS SELECT count(*) FROM docs WHERE body @@@ 'x';
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE c;
SET chdb_search_stub.meta = 'none';
SET chdb_search.unavailable_index = skip;
SELECT * FROM pg_temp.explain($$EXECUTE c$$, true);
EXECUTE c;
RESET chdb_search.unavailable_index;
RESET chdb_search_stub.meta;
RESET plan_cache_mode;
DEALLOCATE c;

DROP TABLE docs;
DROP EXTENSION chdb_search;
