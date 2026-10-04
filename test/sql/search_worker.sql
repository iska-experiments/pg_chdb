-- The chdb_search worker: store, streamed insert, select, drop, recovery.
-- The server needs libchdb on its library path or chdb_search.libchdb_path.
CREATE EXTENSION chdb_search;

SELECT chdb_search_version() ~ '^\d+\.\d+\.\d+$';

-- Start from nothing; DROP is idempotent.
SELECT chdb_search_drop();
SELECT chdb_search_drop();

SELECT chdb_search_exec('CREATE TABLE idx_0.t (id UInt64, body String) ENGINE = MergeTree ORDER BY id');

-- Streamed insert from a heap table, enough rows for several Native blocks.
CREATE TABLE docs AS
SELECT i::bigint AS id, 'row ' || i || ' ' || repeat('x', i % 50) AS body
  FROM generate_series(1, 20000) AS i;
SELECT chdb_search_copy_to('docs', 'INSERT INTO idx_0.t (id, body)');

SELECT * FROM chdb_search_query('SELECT count(), sum(id), max(length(body)) FROM idx_0.t')
    AS (n bigint, total numeric, longest int);
SELECT * FROM chdb_search_query('SELECT id, body FROM idx_0.t WHERE id IN (1, 77, 20000) ORDER BY id')
    AS (id bigint, body text);

-- A bad query reports the worker's error and leaves the worker serving.
SELECT chdb_search_exec('SELECT * FROM idx_0.nope');
SELECT chdb_search_copy_to('docs', 'INSERT INTO idx_0.nope (id, body)');
SELECT * FROM chdb_search_query('SELECT count() FROM idx_0.t') AS (n bigint);

-- Only a superuser may use the debug functions.
CREATE ROLE search_worker_nobody;
SET ROLE search_worker_nobody;
SELECT chdb_search_exec('SELECT 1');
RESET ROLE;
DROP ROLE search_worker_nobody;

-- Kill the worker; the next call starts another that finds the same store.
SELECT pid AS old_pid FROM pg_stat_activity
 WHERE backend_type = 'chdb_search worker' AND datname = current_database() \gset
SELECT pg_terminate_backend(:old_pid);
-- psql variables do not reach inside the DO block's quoting.
SELECT set_config('chdb_search_test.old_pid', :'old_pid', false) IS NOT NULL AS saved;
DO $$
BEGIN
    FOR i IN 1..200 LOOP
        EXIT WHEN NOT EXISTS (SELECT FROM pg_stat_activity WHERE pid = current_setting('chdb_search_test.old_pid')::int);
        PERFORM pg_sleep(0.025);
    END LOOP;
END $$;
SELECT * FROM chdb_search_query('SELECT count() FROM idx_0.t') AS (n bigint);
SELECT pid <> :old_pid AS restarted FROM pg_stat_activity
 WHERE backend_type = 'chdb_search worker' AND datname = current_database();

-- DROP removes the database.
SELECT chdb_search_drop();
SELECT * FROM chdb_search_query('SELECT count() FROM idx_0.t') AS (n bigint);

-- Leave no worker holding the database open for the next run to drop.
SELECT pid AS old_pid FROM pg_stat_activity
 WHERE backend_type = 'chdb_search worker' AND datname = current_database() \gset
SELECT pg_terminate_backend(:old_pid);
DROP TABLE docs;
