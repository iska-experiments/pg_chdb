-- Load the Hacker News dataset once, through the chdb COPY hook, into two
-- identical heaps: hn for the chdb index and hn_pdb for the ParadeDB index,
-- so each engine's writes, REINDEX and VACUUM leave the other's table alone.
--
--   psql -v src=file:///path/hacknernews.parquet [-v limit=10000000] -f load.sql
--
-- src defaults to the public ClickHouse copy of the dataset (28.74M items,
-- 7.1 GB of Parquet); limit, when set, keeps the rows with the lowest ids.
\set ON_ERROR_STOP on
\if :{?src} \else
\set src 'https://datasets-documentation.s3.eu-west-3.amazonaws.com/hackernews/hacknernews.parquet'
\endif
\timing on
SET client_min_messages = warning;
LOAD 'chdb_hook';

DROP TABLE IF EXISTS hn, hn_pdb;
-- The Parquet file's own columns and order; the hook reads them by name.
-- "time" is epoch seconds in the file and converts to timestamptz.
CREATE TABLE hn (
    id          bigint NOT NULL,
    deleted     smallint,
    type        text,
    "by"        text,
    time        timestamptz,
    text        text,
    dead        smallint,
    parent      bigint,
    poll        bigint,
    kids        bigint[],
    url         text,
    score       int,
    title       text,
    parts       bigint[],
    descendants int
);

-- A few values hold NUL bytes, which Postgres text cannot; the hook drops
-- them (encoding_check 'remove') rather than failing the load.
\echo load: COPY hn FROM :'src'
COPY hn FROM :'src' (format 'Parquet', encoding_check 'remove');

\if :{?limit}
\echo load: keeping the :limit rows with the lowest ids
DELETE FROM hn WHERE id > (SELECT id FROM hn ORDER BY id OFFSET :limit - 1 LIMIT 1);
VACUUM FULL hn;
\endif

ALTER TABLE hn ADD PRIMARY KEY (id);
CREATE TABLE hn_pdb (LIKE hn INCLUDING ALL);
INSERT INTO hn_pdb SELECT * FROM hn;

-- All-visible heaps, as both engines' aggregate and visibility shortcuts
-- want, and fresh statistics for the planner.
VACUUM (FREEZE, ANALYZE) hn;
VACUUM (FREEZE, ANALYZE) hn_pdb;
CHECKPOINT;

\timing off
SELECT count(*) AS rows, count(*) FILTER (WHERE text <> '') AS rows_with_text,
       pg_size_pretty(pg_table_size('hn')) AS heap_with_toast,
       min(time) AS first, max(time) AS last
  FROM hn;
