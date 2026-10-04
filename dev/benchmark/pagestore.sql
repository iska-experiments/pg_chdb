-- The WAL a chdb index writes, now that its store lives in the index
-- relation's pages: per row inserted in one transaction of many rows, per
-- commit of a single row (each a flush, a part), and per OPTIMIZE that
-- merges the parts. Run it with psql against a database that has the
-- chdb_search extension installed and libchdb on the server's path:
--
--   psql -v rows=200000 -v commits=200 -f dev/benchmark/pagestore.sql
--
-- To compare with a store outside the pages, run the same script at a
-- commit before "Keep the engine's blobs in index pages": the index then
-- writes only its metapage, and the difference is the store's share.
-- The heap's own WAL is measured apart, so the figures are the index's.
\set ON_ERROR_STOP on
\set QUIET on
\if :{?rows} \else \set rows 200000 \endif
\if :{?commits} \else \set commits 200 \endif
SET search_path = public, chdb;
SET client_min_messages = warning;
DROP TABLE IF EXISTS pagestore_bench;
CREATE TABLE pagestore_bench (id int PRIMARY KEY, body text, author text COLLATE "C", price numeric(10,2));

-- The rows: ~60 words of varied text each, so the text index has work to do.
CREATE OR REPLACE FUNCTION pg_temp.words(seed int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
    SELECT string_agg(
        (ARRAY['running','shoes','walking','boots','hiking','trail','leather','canvas',
               'waterproof','light','heavy','winter','summer','blue','green','red',
               'size','pair','laces','sole','grip','comfort','mountain','city',
               'review','great','poor','value','price','ship'])[1 + ((seed::bigint * 7919 + i * 104729) % 30)],
        ' ')
    FROM generate_series(1, 60) i
$$;

-- The heap alone, for reference.
SELECT pg_current_wal_lsn() AS lsn_heap0 \gset
INSERT INTO pagestore_bench
SELECT i, pg_temp.words(i), 'author ' || (i % 1000), (i % 10000) / 100.0
  FROM generate_series(1, :rows) i;
SELECT pg_current_wal_lsn() AS lsn_heap1 \gset

-- The build.
SELECT pg_current_wal_lsn() AS lsn_build0 \gset
CREATE INDEX pagestore_bench_idx ON pagestore_bench USING chdb (body text_ops, author columnar_ops, price columnar_ops);
SELECT pg_current_wal_lsn() AS lsn_build1 \gset

-- One transaction of many rows: one flush, one part (plus staging past the threshold).
SELECT pg_current_wal_lsn() AS lsn_bulk0 \gset
INSERT INTO pagestore_bench
SELECT :rows + i, pg_temp.words(:rows + i), 'author ' || (i % 1000), (i % 10000) / 100.0
  FROM generate_series(1, :rows) i;
SELECT pg_current_wal_lsn() AS lsn_bulk1 \gset

-- Single-row commits: each a flush, each a part.
SELECT pg_current_wal_lsn() AS lsn_row0 \gset
SELECT format('INSERT INTO pagestore_bench VALUES (%s, %L, %L, %s)',
              2 * :rows + i, pg_temp.words(2 * :rows + i), 'author ' || (i % 1000), (i % 10000) / 100.0)
  FROM generate_series(1, :commits) i \gexec
SELECT pg_current_wal_lsn() AS lsn_row1 \gset

-- OPTIMIZE FINAL merges every part into one, as VACUUM's does past its ratio.
SELECT chdb_search_store_table('pagestore_bench_idx') AS tbl \gset
SELECT count(*) AS parts_before FROM chdb_search_blobs('pagestore_bench_idx') WHERE key ~ '/count\.txt$' \gset
SELECT pg_current_wal_lsn() AS lsn_opt0 \gset
SELECT chdb_search_exec(format('OPTIMIZE TABLE %s FINAL', :'tbl'));
SELECT pg_current_wal_lsn() AS lsn_opt1 \gset

\set QUIET off
\pset format aligned
\pset tuples_only off
SELECT :rows AS rows_per_batch, :commits AS single_row_commits,
       pg_size_pretty(pg_relation_size('pagestore_bench')) AS heap,
       pg_size_pretty(pg_relation_size('pagestore_bench_idx')) AS index_relation,
       pg_size_pretty(sum(size)::bigint) AS blobs,
       count(*) AS blob_count,
       count(*) FILTER (WHERE size <= 2048) AS inline_blobs
  FROM chdb_search_blobs('pagestore_bench_idx');
SELECT step, pg_size_pretty(bytes) AS wal, pg_size_pretty((bytes / rows)::bigint) AS wal_per_row
  FROM (VALUES
    ('heap insert, no index',   :'lsn_heap1'::pg_lsn - :'lsn_heap0'::pg_lsn,   :rows::numeric),
    ('index build',             :'lsn_build1'::pg_lsn - :'lsn_build0'::pg_lsn, :rows::numeric),
    ('bulk insert (heap+index)',:'lsn_bulk1'::pg_lsn - :'lsn_bulk0'::pg_lsn,   :rows::numeric),
    ('single-row commits',      :'lsn_row1'::pg_lsn - :'lsn_row0'::pg_lsn,     :commits::numeric),
    ('OPTIMIZE FINAL',          :'lsn_opt1'::pg_lsn - :'lsn_opt0'::pg_lsn,     (2 * :rows + :commits)::numeric)
  ) AS t(step, bytes, rows);
SELECT :parts_before AS parts_before_optimize,
       (SELECT count(*) FROM chdb_search_blobs('pagestore_bench_idx') WHERE key ~ '/count\.txt$') AS parts_after;
DROP TABLE pagestore_bench;
