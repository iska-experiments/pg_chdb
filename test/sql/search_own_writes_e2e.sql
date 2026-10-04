-- A transaction sees its own rows through a chdb index, end to end: a
-- search ships the rows the transaction has buffered to a staging table,
-- idx_<oid>.t_<generation>_tx_<xid>, and reads it with the index's table;
-- the heap fetch hides the old versions and the deleted rows as ever.
-- search_own_writes pins the statements in both client modes. The server
-- needs libchdb on its library path, for chdb_search_engine.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;
SET chdb_search.mask_oids = on;
-- The index access method's own scans are under test: the custom scan,
-- which reads the same union (search_planner), is kept out of the plans.
SET chdb_search.enable_custom_scan = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;

-- The index's ClickHouse table, and the count of the store's generation and
-- staging tables, read through the worker.
CREATE FUNCTION pg_temp.store(text)
RETURNS TABLE (ctid bigint, body text, tags text[], price numeric)
LANGUAGE sql AS $$
    SELECT * FROM chdb_search_query(format(
        'SELECT toInt64(ctid), body, tags, price FROM %s ORDER BY ctid', $1
    )) AS (ctid bigint, body text, tags text[], price numeric)
$$;
CREATE FUNCTION pg_temp.tables(oid)
RETURNS bigint
LANGUAGE sql AS $$
    SELECT n FROM chdb_search_query(format(
        'SELECT count() FROM system.tables WHERE database = ''idx_%s'' AND name LIKE ''t\_%%''', $1
    )) AS (n bigint)
$$;

CREATE TABLE own (id int PRIMARY KEY, body text, tags text[], price numeric(10, 2));
INSERT INTO own VALUES (1, 'Running shoes for runners', '{sport}', 49.90), (2, 'Walking boots', '{outdoor}', 89.00);
CREATE INDEX own_idx ON own USING chdb (body, tags text_array_ops, price columnar_ops);
SELECT 'own_idx'::regclass::oid AS own_oid \gset
SELECT chdb_search_store_table('own_idx') AS own_tbl \gset
BEGIN;
INSERT INTO own VALUES (3, 'staged shoes', '{own}', 1);
EXPLAIN (COSTS OFF) SELECT id FROM own WHERE body @@@ 'staged';
SELECT id FROM own WHERE body @@@ 'staged';
SELECT id FROM own WHERE body @@@ 'shoes' ORDER BY id;
SELECT id FROM own WHERE tags @@= 'own' AND price < 2 ORDER BY id;
-- An UPDATE through the index of a row inserted in this transaction: the new
-- version is found afterwards, the old one no longer.
UPDATE own SET body = 'staged sandals' WHERE body @@@ 'staged shoes';
SELECT id, body FROM own WHERE body @@@ 'staged' ORDER BY id;
SELECT id FROM own WHERE body @@@ 'shoes' ORDER BY id;
-- A DELETE through the index of such a row.
INSERT INTO own VALUES (4, 'doomed boots', '{own}', 2);
SELECT id FROM own WHERE body @@@ 'boots' ORDER BY id;
DELETE FROM own WHERE body @@@ 'doomed';
SELECT id FROM own WHERE body @@@ 'boots' ORDER BY id;
-- A savepoint rolled back after its rows were staged hides them at once.
SAVEPOINT s;
INSERT INTO own VALUES (5, 'savepoint shoes', '{own}', 3);
SELECT id FROM own WHERE body @@@ 'savepoint';
ROLLBACK TO s;
SELECT id FROM own WHERE body @@@ 'savepoint';
SELECT id FROM own WHERE body @@@ 'shoes' ORDER BY id;
-- COPY stages like INSERT.
COPY own (id, body) FROM stdin;
6	copied shoes
\.
SELECT id FROM own WHERE body @@@ 'shoes' ORDER BY id;
-- The staging table stands beside the index's table until COMMIT.
SELECT pg_temp.tables(:own_oid);
COMMIT;
-- Committed, the rows are in the index's table, less the rolled-back one,
-- and the staging table is gone.
SELECT pg_temp.tables(:own_oid);
SELECT ctid, body FROM pg_temp.store(:'own_tbl') ORDER BY ctid;
SELECT id FROM own WHERE body @@@ 'shoes' ORDER BY id;
-- A rolled-back transaction drops its staging table, and its rows never
-- reach the index's table.
BEGIN;
INSERT INTO own VALUES (7, 'rolled back shoes', '{gone}', 1);
SELECT id FROM own WHERE body @@@ 'rolled';
ROLLBACK;
SELECT id FROM own WHERE body @@@ 'rolled';
SELECT pg_temp.tables(:own_oid);
SELECT count(*) FROM pg_temp.store(:'own_tbl') WHERE body LIKE 'rolled%';
-- Past flush_threshold rows are staged as they come, and every search in
-- between sees them all.
SET chdb_search.flush_threshold = '64kB';
BEGIN;
INSERT INTO own (id, body) SELECT 1000 + i, 'bulk ' || repeat('word ', 250) FROM generate_series(1, 120) i;
SELECT count(*) FROM own WHERE body @@@ 'bulk';
INSERT INTO own (id, body) VALUES (2000, 'bulk tail');
SELECT count(*) FROM own WHERE body @@@ 'bulk';
COMMIT;
RESET chdb_search.flush_threshold;
SELECT count(*) FROM own WHERE body @@@ 'bulk';
SELECT count(*) FROM pg_temp.store(:'own_tbl') WHERE body LIKE 'bulk%';
DROP TABLE own;

DROP EXTENSION chdb_search;

-- The worker stays connected to the database, which could not be dropped for
-- the next run, so stop it.
DO $$
BEGIN
    PERFORM pg_terminate_backend(pid, 10000)
       FROM pg_stat_activity
      WHERE backend_type = 'chdb_search worker' AND datname = current_database();
END $$;
