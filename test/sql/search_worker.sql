-- The chdb_search worker: store, streamed insert, select, drop, recovery.
-- The server needs libchdb on its library path, for chdb_search_engine.

-- A setting made before the library loads survives its GUC definitions.
SET chdb_search.worker_timeout = '45s';
CREATE EXTENSION chdb_search;
SHOW chdb_search.worker_timeout;
RESET chdb_search.worker_timeout;
SET search_path = public, chdb;

SELECT chdb_search_version() ~ '^\d+\.\d+\.\d+$';

-- Start from nothing; DROP is idempotent.
SELECT chdb_search_drop();
SELECT chdb_search_drop();

-- The index's database is made on the first request for it, and a DROP
-- DATABASE run as a statement is a drop too: the next request makes it again.
SELECT * FROM chdb_search_query('SELECT count() FROM system.databases WHERE name = ''idx_0''') AS (n bigint);
SELECT chdb_search_exec('DROP DATABASE idx_0 SYNC');
SELECT * FROM chdb_search_query('SELECT count() FROM system.databases WHERE name = ''idx_0''') AS (n bigint);

SELECT chdb_search_exec('CREATE TABLE idx_0.t (id UInt64, body String) ENGINE = MergeTree ORDER BY id');

-- A request names the store generation of the table it works on, zero for
-- none. The worker checks that idx_0.t_<generation> exists before running it,
-- so a statement against a store that is gone or rebuilt fails clearly.
SELECT chdb_search_exec('SELECT 1', 7);
SELECT chdb_search_exec('CREATE TABLE idx_0.t_7 (id UInt64) ENGINE = MergeTree ORDER BY id');
SELECT chdb_search_exec('SELECT 1', 7);

-- Streamed insert from a heap table, enough rows for several Native blocks.
CREATE TABLE docs AS
SELECT i::bigint AS id, 'row ' || i || ' ' || repeat('x', i % 50) AS body
  FROM generate_series(1, 20000) AS i;
SELECT chdb_search_copy_to('docs', 'INSERT INTO idx_0.t (id, body)');

SELECT * FROM chdb_search_query('SELECT count(), sum(id), max(length(body)) FROM idx_0.t')
    AS (n bigint, total numeric, longest int);
SELECT * FROM chdb_search_query('SELECT id, body FROM idx_0.t WHERE id IN (1, 77, 20000) ORDER BY id')
    AS (id bigint, body text);

-- The session takes the limits each request carries, like chdb and chdb_hook.
-- A limit of zero reads back as auto(N) with the machine's thread count.
PREPARE show_limits AS
SELECT * FROM chdb_search_query($$
    SELECT name, replaceRegexpOne(value, '[(][0-9]+[)]$', '') AS value
      FROM system.settings
     WHERE name IN ('max_memory_usage', 'max_threads', 'max_parsing_threads')
     ORDER BY name
$$) AS (name text, value text);
SET chdb_search.max_memory = '100MB';
SET chdb_search.max_threads = 3;
SET chdb_search.max_parsing_threads = 2;
EXECUTE show_limits;
RESET chdb_search.max_memory;
RESET chdb_search.max_threads;
RESET chdb_search.max_parsing_threads;
EXECUTE show_limits;

-- A bad query reports the worker's error and leaves the worker serving.
SELECT chdb_search_exec('SELECT * FROM idx_0.nope');
SELECT chdb_search_copy_to('docs', 'INSERT INTO idx_0.nope (id, body)');
SELECT * FROM chdb_search_query('SELECT count() FROM idx_0.t') AS (n bigint);

-- A select ClickHouse rejects raises, rather than finding nothing.
SELECT * FROM chdb_search_query('SELECT count() FROM idx_0.nope') AS (n bigint);

-- EXECUTE on the debug functions is revoked from PUBLIC.
CREATE ROLE search_worker_nobody;
SET ROLE search_worker_nobody;
SELECT chdb_search_exec('SELECT 1');
SELECT chdb_search_engine_pid();
SELECT chdb_search_debug_kill_engine(9);
RESET ROLE;
DROP ROLE search_worker_nobody;

-- libchdb runs in a child of the worker, so a crash in it costs the request in
-- flight and nothing else. Crash it with SIGSEGV: the next request fails naming
-- the signal, the one after works, and Postgres never noticed.
SELECT pg_backend_pid() AS backend_pid, pg_postmaster_start_time() AS started,
       (SELECT pid FROM pg_stat_activity
         WHERE backend_type = 'chdb_search worker' AND datname = current_database()) AS worker_pid \gset
SELECT chdb_search_engine_pid() AS engine_pid \gset
SELECT :engine_pid > 0 AS engine_runs;
SELECT chdb_search_debug_kill_engine(11) = :engine_pid AS signalled;
DO $$
DECLARE
    detail text;
BEGIN
    PERFORM count(*) FROM chdb_search_query('SELECT count() FROM idx_0.t') AS (n bigint);
EXCEPTION WHEN OTHERS THEN
    GET STACKED DIAGNOSTICS detail = PG_EXCEPTION_DETAIL;
    RAISE NOTICE '%', regexp_replace(detail, 'pid \d+', 'pid N');
END $$;
SELECT * FROM chdb_search_query('SELECT count() FROM idx_0.t') AS (n bigint);
SELECT chdb_search_engine_pid() <> :engine_pid AS respawned;

-- No crash recovery: same backend, same postmaster, same worker.
SELECT pg_backend_pid() = :backend_pid AS same_backend,
       pg_postmaster_start_time() = :'started' AS same_postmaster,
       (SELECT pid FROM pg_stat_activity
         WHERE backend_type = 'chdb_search worker' AND datname = current_database()) = :worker_pid AS same_worker;

-- Kill the worker; the next call starts another, which empties the engine's
-- directory: the store tables of chdb indexes are attached again from the
-- catalog on demand, but the debug database is gone with it.
SELECT pid AS old_pid FROM pg_stat_activity
 WHERE backend_type = 'chdb_search worker' AND datname = current_database() \gset
SELECT pg_terminate_backend(:old_pid);
-- psql variables do not reach inside the DO block's quoting.
SELECT set_config('chdb_search_test.old_pid', :'old_pid', false) IS NOT NULL AS saved;
DO $$
BEGIN
    FOR i IN 1..200 LOOP
        -- pg_stat_activity is cached for the transaction; read it afresh.
        PERFORM pg_stat_clear_snapshot();
        EXIT WHEN NOT EXISTS (SELECT FROM pg_stat_activity WHERE pid = current_setting('chdb_search_test.old_pid')::int);
        PERFORM pg_sleep(0.025);
    END LOOP;
END $$;
SELECT * FROM chdb_search_query('SELECT count() FROM system.tables WHERE database = ''idx_0''') AS (n bigint);
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

-- Leave the database as other tests expect to find it.
DROP EXTENSION chdb_search;
