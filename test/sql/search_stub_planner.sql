-- The chdb_search custom scan with the stub worker client (make
-- CHDB_SEARCH_STUB=1), which answers a statement with the ctids
-- chdb_search_stub.ctids names: the plans, and what the scan does with the
-- rows, seen without a worker. A ctid is packed as (block << 16) | offset,
-- so (0,1) is 1 and (1,1) is 65537. Runs only with the stub (see the
-- Makefile); search_planner covers the worker.
\set VERBOSITY terse
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;
SET chdb_search.mask_oids = on;

CREATE TABLE docs (id int PRIMARY KEY, body text, price numeric(10, 2), loc point);
INSERT INTO docs VALUES
    (1, 'Running shoes', 49.90, '(0,0)'), (2, 'Walking boots', 89.00, '(10,10)'),
    (3, 'Trail shoes', 120.00, '(1,1)');
ALTER OPERATOR FAMILY columnar_ops USING chdb ADD OPERATOR 1 <-> (point, point) FOR ORDER BY float_ops;
CREATE INDEX docs_idx ON docs USING chdb (body, price, loc);
SET enable_seqscan = off;
SET enable_bitmapscan = off;
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
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'running shoes';
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE chdb.has_all_tokens(body, 'running shoes') AND price < 100 AND id > 1;
EXPLAIN (COSTS OFF) SELECT id FROM docs ORDER BY loc <-> '(0,0)' LIMIT 2;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'shoes' AND id > 1 ORDER BY loc <-> '(0,0)' LIMIT 2;
-- The query language: a chdb.query tree, a regex and a wildcard.
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ (chdb.match('running') || !chdb.term('trail'));
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@/ '^run' AND chdb.wildcard(body, 'trail%') ORDER BY loc <-> '(0,0)' LIMIT 1;
SET chdb_search.enable_custom_scan = off;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'running shoes';
RESET chdb_search.enable_custom_scan;

----------------------------------------------------------------------------
-- The rows the store returns are fetched from the heap by ctid
----------------------------------------------------------------------------
SET chdb_search_stub.ctids = '1,2';
SELECT id FROM docs WHERE body @@@ 'x' ORDER BY id;
-- The scan's own filter applies to the heap row.
SELECT id FROM docs WHERE body @@@ 'x' AND id > 1;
-- The heap fetch decides visibility: a deleted row's ctid finds nothing...
DELETE FROM docs WHERE id = 2;
SELECT id FROM docs WHERE body @@@ 'x';
-- ...and a pushed LIMIT the heap thinned asks the store again, which
-- answers the same, so the scan stops once nothing new comes.
SELECT * FROM pg_temp.explain($$SELECT id FROM docs ORDER BY loc <-> '(0,0)' LIMIT 2$$, true);
-- A block at or past the heap's end is skipped rather than read.
SET chdb_search_stub.ctids = '1,4294967296,65537';
SELECT id FROM docs WHERE body @@@ 'x';
-- No rows at all, as the stub answers by default.
RESET chdb_search_stub.ctids;
SELECT id FROM docs WHERE body @@@ 'x';
-- A worker lost mid-answer raises.
SET chdb_search_stub.ctids = '1';
SET chdb_search_stub.fail = on;
SELECT id FROM docs WHERE body @@@ 'x';
RESET chdb_search_stub.fail;

----------------------------------------------------------------------------
-- The fail-safe check runs before the statement: a store that does not
-- match the metapage is refused, or skipped at planning
----------------------------------------------------------------------------
-- The scans above proved the store for this state of the index, and the
-- verdict is kept with the index's relcache entry: altering the index drops
-- it, so that the store is asked again.
ALTER INDEX docs_idx SET (vacuum_optimize_ratio = 0.5);
SET chdb_search_stub.meta = 'none';
SELECT id FROM docs WHERE body @@@ 'x';
-- In skip mode neither the index nor the custom scan is offered.
SET chdb_search.unavailable_index = skip;
SET enable_seqscan = on;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'x';
SELECT id FROM docs WHERE body @@@ 'x';
SET enable_seqscan = off;
RESET chdb_search.unavailable_index;
RESET chdb_search_stub.meta;
SELECT id FROM docs WHERE body @@@ 'x';

DROP TABLE docs;
ALTER OPERATOR FAMILY columnar_ops USING chdb DROP OPERATOR 1 (point, point);
DROP EXTENSION chdb_search;
