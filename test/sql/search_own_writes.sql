-- A transaction sees its own rows through a chdb index. A search ships the
-- rows the transaction has buffered for the index to its staging table,
-- idx_<oid>.t_<generation>_tx_<xid>, and reads that table with the index's
-- in one query; COMMIT then attaches the staged parts to the table. The
-- searches below match nothing, so that the output is the same with the
-- worker and with the stub client (see search_am.sql), which returns no
-- rows; what a transaction sees is checked by search_e2e.
\set VERBOSITY terse
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;
SET chdb_search.mask_oids = on;
-- The index access method's own scans are under test first: the custom
-- scan, which reads the same union, is kept out of the plans until the end.
SET chdb_search.enable_custom_scan = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;

ALTER OPERATOR FAMILY columnar_ops USING chdb ADD OPERATOR 1 <-> (point, point) FOR ORDER BY float_ops;
CREATE TABLE docs (id int PRIMARY KEY, body text, loc point);
INSERT INTO docs VALUES (1, 'Running shoes for runners', '(0,0)'), (2, 'Walking boots', '(1,1)');
CREATE INDEX docs_idx ON docs USING chdb (body, loc);

----------------------------------------------------------------------------
-- A search stages the rows buffered so far and reads both tables
----------------------------------------------------------------------------
SET client_min_messages = debug1;
BEGIN;
INSERT INTO docs (id, body) VALUES (3, 'staged shoes');
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'nomatch';
SELECT id FROM docs WHERE body @@@ 'nomatch';
\echo -- nothing new to ship: the union alone
SELECT id FROM docs WHERE body @@@ 'nomatch';
\echo -- the rows of each statement since go as one more block, however many
INSERT INTO docs (id, body) VALUES (4, 'staged boots'), (5, 'staged sandals');
UPDATE docs SET body = 'updated through the index' WHERE body @@@ 'nomatch';
\echo -- a distance ORDER BY orders and limits each leg, then the union
SELECT id FROM docs WHERE body @@@ 'nomatch' ORDER BY loc <-> '(0,0)' LIMIT 2;
\echo -- COMMIT sends what is still buffered, attaches the staged parts, drops
INSERT INTO docs (id, body) VALUES (6, 'unread shoes');
COMMIT;
\echo -- a transaction that never searches sends one block at COMMIT, as before
BEGIN;
INSERT INTO docs (id, body) VALUES (7, 'unread boots');
COMMIT;
\echo -- ROLLBACK drops the staging table
BEGIN;
INSERT INTO docs (id, body) VALUES (8, 'rolled back');
SELECT id FROM docs WHERE body @@@ 'nomatch';
ROLLBACK;

----------------------------------------------------------------------------
-- A savepoint rolled back after its rows were staged is excluded, from the
-- searches and from the commit
----------------------------------------------------------------------------
BEGIN;
INSERT INTO docs (id, body) VALUES (9, 'outer');
SAVEPOINT s;
INSERT INTO docs (id, body) VALUES (10, 'inner');
SELECT id FROM docs WHERE body @@@ 'nomatch';
ROLLBACK TO s;
SELECT id FROM docs WHERE body @@@ 'nomatch';
COMMIT;

----------------------------------------------------------------------------
-- COPY, and a transaction past flush_threshold, stage the same way
----------------------------------------------------------------------------
BEGIN;
COPY docs (id, body) FROM stdin;
11	copied shoes
12	copied boots
\.
SELECT id FROM docs WHERE body @@@ 'nomatch';
COMMIT;
SET chdb_search.flush_threshold = '64kB';
BEGIN;
INSERT INTO docs (id, body) SELECT 1000 + i, repeat('word ', 250) FROM generate_series(1, 120) i;
SELECT id FROM docs WHERE body @@@ 'nomatch';
INSERT INTO docs (id, body) VALUES (2000, 'tail');
SELECT id FROM docs WHERE body @@@ 'nomatch';
COMMIT;
RESET chdb_search.flush_threshold;

----------------------------------------------------------------------------
-- The custom scan's statement reads the same union, the pushed LIMIT on
-- each leg and on the merge
----------------------------------------------------------------------------
SET chdb_search.enable_custom_scan = on;
BEGIN;
INSERT INTO docs (id, body) VALUES (3000, 'custom shoes');
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'nomatch' ORDER BY loc <-> '(0,0)' LIMIT 2;
SELECT id FROM docs WHERE body @@@ 'nomatch' ORDER BY loc <-> '(0,0)' LIMIT 2;
COMMIT;
RESET client_min_messages;

DROP TABLE docs;
ALTER OPERATOR FAMILY columnar_ops USING chdb DROP OPERATOR 1 (point, point);
DROP EXTENSION chdb_search;

-- The worker stays connected to the database, which could not be dropped for
-- the next run, so stop it. There is none with the stub client.
DO $$
BEGIN
    PERFORM pg_terminate_backend(pid, 10000)
       FROM pg_stat_activity
      WHERE backend_type = 'chdb_search worker' AND datname = current_database();
END $$;
