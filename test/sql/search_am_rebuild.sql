-- Rebuilds of a chdb index. REINDEX, TRUNCATE, CLUSTER and table rewrites all
-- reach ambuild for an index that keeps its OID. Each build creates a store
-- table named after the new metapage generation, idx_N.t_N, beside the old
-- one, so a rolled-back rebuild leaves the table the index still names
-- untouched, and VACUUM drops the generation that lost. The output is the
-- same with the worker and with the stub client (see search_am.sql); what the
-- store holds after each step is checked by search_e2e.
\set VERBOSITY terse
CREATE EXTENSION chdb_search;
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
