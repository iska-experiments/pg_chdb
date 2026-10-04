-- Rebuilds of a chdb index. REINDEX, TRUNCATE, CLUSTER and table rewrites all
-- reach ambuild for an index that keeps its OID. Each build creates a store
-- table named after the new metapage generation, idx_N.t_N, beside the old
-- one, so a rolled-back rebuild leaves the table the index still names
-- untouched, and VACUUM drops the generation that lost. The output is the
-- same with the worker and with the stub client (see search_am.sql); what the
-- store holds after each step is checked by search_e2e.
\set VERBOSITY terse
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;
SET chdb_search.mask_oids = on;

-- (Bounded varchars keep the table free of a TOAST table, whose rebuilds
-- would log its OID at DEBUG1.)
CREATE TABLE docs (id int PRIMARY KEY, body varchar(100), title varchar(100));
INSERT INTO docs VALUES (1, 'Running shoes for runners', 'Shoes'), (2, 'Walking boots', 'Boots');
CREATE INDEX docs_idx ON docs USING chdb (body text_ops);
CREATE INDEX docs_title ON docs USING chdb (title text_ops);

----------------------------------------------------------------------------
-- A rolled-back rebuild drops the generation it wrote and nothing else
----------------------------------------------------------------------------
SET client_min_messages = debug1;
BEGIN;
REINDEX INDEX docs_idx;
ROLLBACK;
\echo -- TRUNCATE rebuilds every index of the table
BEGIN;
TRUNCATE docs;
ROLLBACK;
SELECT count(*) FROM docs;
\echo -- rolled back to a savepoint
BEGIN;
SAVEPOINT s;
REINDEX TABLE docs;
ROLLBACK TO s;
COMMIT;
\echo -- a table rewrite (of a column the index does not cover, so it is rebuilt)
BEGIN;
ALTER TABLE docs ALTER COLUMN id TYPE bigint;
ROLLBACK;
\echo -- CLUSTER
BEGIN;
CLUSTER docs USING docs_pkey;
ROLLBACK;
\echo -- a search reads the table the index still names
SET enable_seqscan = off;
\o /dev/null
SELECT id FROM docs WHERE body @@@ 'running shoes';
\o
RESET enable_seqscan;

----------------------------------------------------------------------------
-- VACUUM sweeps the generations the index does not name
----------------------------------------------------------------------------
\echo -- nothing to sweep yet: every rebuild above was undone
VACUUM docs;
\echo -- a committed rebuild leaves the old generation to the next VACUUM
REINDEX INDEX docs_idx;
RESET client_min_messages;
-- (With the worker, VACUUM now drops the previous generation; the stub
-- client returns no table names, so the statement is checked by search_e2e.)
VACUUM docs;

----------------------------------------------------------------------------
-- A rebuild takes over the rows the transaction had buffered for the index
----------------------------------------------------------------------------
SET client_min_messages = debug1;
\echo -- the build scan indexes the row, so COMMIT sends it to docs_title only
BEGIN;
INSERT INTO docs (id, body) VALUES (9, 'running shoes');
REINDEX INDEX docs_idx;
COMMIT;
\echo -- TRUNCATE rebuilds both indexes: nothing is sent at COMMIT
BEGIN;
INSERT INTO docs (id, body) VALUES (10, 'running shoes again');
TRUNCATE docs;
COMMIT;
\echo -- a rebuild rolled back to a savepoint gives the rows back
BEGIN;
INSERT INTO docs (id, body) VALUES (11, 'kept running');
SAVEPOINT s;
REINDEX INDEX docs_idx;
INSERT INTO docs (id, body) VALUES (12, 'rolled back with the rebuild');
ROLLBACK TO s;
COMMIT;
\echo -- a released one keeps them
BEGIN;
INSERT INTO docs (id, body) VALUES (13, 'taken by the rebuild');
SAVEPOINT s;
REINDEX INDEX docs_idx;
RELEASE s;
INSERT INTO docs (id, body) VALUES (14, 'sent to the new table');
COMMIT;
\echo -- control: one insert per index
BEGIN;
INSERT INTO docs (id, body) VALUES (15, 'control');
COMMIT;
RESET client_min_messages;

----------------------------------------------------------------------------
-- Two-phase commit: a store drop cannot be carried past PREPARE
----------------------------------------------------------------------------
-- (max_prepared_transactions is 0 here, which is checked only after this.)
SET client_min_messages = debug1;
BEGIN;
DROP INDEX docs_title;
PREPARE TRANSACTION 'p';
\echo -- a new index is refused too, and its store dropped with the transaction
BEGIN;
CREATE INDEX docs_prep ON docs USING chdb (title text_ops);
PREPARE TRANSACTION 'q';
\echo -- buffered rows are refused by the buffer
BEGIN;
INSERT INTO docs (id, body) VALUES (16, 'x');
PREPARE TRANSACTION 'r';
\echo -- a rebuild may be prepared: the abort would only have dropped a table VACUUM sweeps
-- (The setting refuses it here, and the abort drops the generation the
-- rebuild wrote from the failing statement, whose error and that line can
-- reach the client in either order. SET LOCAL quiets the statement until
-- the abort callbacks have run; t/search_prepare.pl pins the drop through
-- the server log.)
BEGIN;
REINDEX INDEX docs_idx;
SET LOCAL client_min_messages = warning;
PREPARE TRANSACTION 's';
SELECT count(*) FROM pg_prepared_xacts;
\echo -- nothing deferred is left behind in the backend
BEGIN;
SELECT 1/0;
ROLLBACK;
RESET client_min_messages;

----------------------------------------------------------------------------
-- The rest of an index's life: concurrent builds, expression columns, the
-- metapage, options, and drops by transaction, cascade and partition
----------------------------------------------------------------------------
-- CREATE INDEX CONCURRENTLY builds the store, then validates the index
-- against it and inserts what it finds missing, which depends on the
-- client's answers, so those statements are not pinned; REINDEX INDEX
-- CONCURRENTLY gives the index a new OID and drops the old store.
CREATE INDEX CONCURRENTLY docs_cic ON docs USING chdb (body text_ops);
SELECT indisvalid FROM pg_index WHERE indexrelid = 'docs_cic'::regclass;
REINDEX INDEX CONCURRENTLY docs_cic;
SELECT indisvalid FROM pg_index WHERE indexrelid = 'docs_cic'::regclass;
SET client_min_messages = debug1;
DROP INDEX docs_cic;
\echo -- expression columns get distinct names; one named as the store names its own is refused
CREATE INDEX docs_expr ON docs USING chdb ((lower(body)), (lower(title)));
DROP INDEX docs_expr;
RESET client_min_messages;
CREATE FUNCTION ctid(text) RETURNS text LANGUAGE sql IMMUTABLE AS $$ SELECT $1 $$;
CREATE INDEX ON docs USING chdb ((ctid(body)));
DROP FUNCTION ctid(text);
\echo -- the metapage: the magic CHDS, version 1, a generation, the last flush
SELECT to_hex(magic), version, generation <> '0', flushed_lsn > '0/0'
  FROM chdb_search_metapage('docs_idx');
SELECT * FROM chdb_search_metapage('docs_pkey');
\echo -- the index option is set and reset in place, without a build
SET client_min_messages = debug1;
ALTER INDEX docs_idx SET (vacuum_optimize_ratio = 0.9);
SELECT reloptions FROM pg_class WHERE relname = 'docs_idx';
ALTER INDEX docs_idx RESET (vacuum_optimize_ratio);
RESET client_min_messages;
\echo -- an insert and a drop of one index in a transaction: the row reaches every store, then the dropped one goes
CREATE INDEX docs_tmp ON docs USING chdb (title text_ops);
SET client_min_messages = debug1;
BEGIN;
INSERT INTO docs (id) VALUES (30);
DROP INDEX docs_tmp;
COMMIT;
RESET client_min_messages;
\echo -- dropped with its schema
CREATE SCHEMA s;
CREATE TABLE s.t (b varchar(50));
CREATE INDEX ON s.t USING chdb (b);
SET client_min_messages = debug1;
DROP SCHEMA s CASCADE;
RESET client_min_messages;
\echo -- one store per partition and none for the parent, each dropped with its table
CREATE TABLE p (b varchar(50)) PARTITION BY LIST (b);
CREATE TABLE p1 PARTITION OF p FOR VALUES IN ('x');
CREATE TABLE p2 PARTITION OF p FOR VALUES IN ('y');
SET client_min_messages = debug1;
CREATE INDEX ON p USING chdb (b);
DROP TABLE p;
RESET client_min_messages;

----------------------------------------------------------------------------
-- DROP INDEX CONCURRENTLY drops the store once, after its last internal commit
----------------------------------------------------------------------------
CREATE INDEX docs_cic ON docs USING chdb (body text_ops);
SET client_min_messages = debug1;
DROP INDEX CONCURRENTLY docs_cic;
RESET client_min_messages;

----------------------------------------------------------------------------
-- DROP INDEX drops the whole store at commit, a rolled-back one does not
----------------------------------------------------------------------------
SET client_min_messages = debug1;
BEGIN;
DROP INDEX docs_title;
ROLLBACK;
DROP INDEX docs_title;
\echo -- a rolled-back CREATE INDEX drops its store
BEGIN;
CREATE INDEX docs_rolled ON docs USING chdb (title text_ops);
ROLLBACK;
DROP TABLE docs;
RESET client_min_messages;

DROP EXTENSION chdb_search;

-- The worker stays connected to the database, which could not be dropped for
-- the next run, so stop it. There is none with the stub client.
DO $$
BEGIN
    PERFORM pg_terminate_backend(pid, 10000)
       FROM pg_stat_activity
      WHERE backend_type = 'chdb_search worker' AND datname = current_database();
END $$;
