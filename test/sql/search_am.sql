-- Tests of the statements the chdb index access method generates, logged at
-- DEBUG1. The output is the same with the worker and with the stub client
-- (make CHDB_SEARCH_STUB=1), which accepts everything and returns no rows: rows
-- found through the index are discarded here and checked by search_e2e.
\set VERBOSITY terse
-- The extension installs into schema chdb, which it creates or, as here,
-- finds already in use, and which DROP EXTENSION then leaves alone.
CREATE SCHEMA IF NOT EXISTS chdb;
CREATE TABLE chdb.user_table (x int);
CREATE EXTENSION chdb_search;
SELECT extnamespace::regnamespace FROM pg_extension WHERE extname = 'chdb_search';
SET search_path = public, chdb;
SET chdb_search.mask_oids = on;

SELECT chdb_search_version() ~ '^\d+\.\d+\.\d+$';

----------------------------------------------------------------------------
-- Predicates: the Postgres fallback of the default tokenizer
----------------------------------------------------------------------------
SELECT chdb.has_all_tokens('The Quick brown fox', 'quick FOX'),
       chdb.has_all_tokens('The Quick brown fox', 'quick cat'),
       chdb.has_all_tokens('The Quick brown fox', ''),
       chdb.has_any_tokens('The Quick brown fox', 'cat FOX'),
       chdb.has_any_tokens('The Quick brown fox', 'cat dog'),
       chdb.has_token('snake_case-name', 'case'),
       chdb.has_token('snake_case-name', 'cas'),
       chdb.has_phrase('a quick brown fox', 'Quick, Brown'),
       chdb.has_phrase('a quick brown fox', 'brown quick');
SELECT chdb.has_token('x', 'two tokens');
SELECT 'Wörter und Zahlen 42' @@@ 'wörter 42',
       'one two' @@? 'three two',
       'one two' @@~ 'one two',
       'one two' @@= 'two',
       ARRAY['Red', 'Blue'] @@@ 'red',
       ARRAY['Red', 'Blue'] @@? 'green',
       ARRAY['Red', 'Blue'] @@= 'BLUE',
       chdb.has_all_tokens(ARRAY['New York'], 'new york');

----------------------------------------------------------------------------
-- Operator classes validate
----------------------------------------------------------------------------
SELECT opcname, amvalidate(oid)
  FROM pg_opclass
 WHERE opcmethod = (SELECT oid FROM pg_am WHERE amname = 'chdb')
 ORDER BY opcname;

SELECT opcname, opcdefault, opcintype::regtype
  FROM pg_opclass
 WHERE opcmethod = (SELECT oid FROM pg_am WHERE amname = 'chdb')
 ORDER BY opcname;

----------------------------------------------------------------------------
-- CREATE INDEX and its options
----------------------------------------------------------------------------
CREATE TABLE docs (
    id int PRIMARY KEY,
    body text,
    title text,
    tags text[],
    author text,
    price numeric(10, 2),
    seen timestamptz,
    flag bool
);
INSERT INTO docs VALUES
    (1, 'Running shoes for runners', 'Shoes', '{sport,shoes}', 'ann', 49.90, '2026-01-02 03:04:05+00', true),
    (2, 'Walking boots', 'Boots', '{outdoor}', 'bob', 89.00, '2026-02-03 00:00:00+00', false);

SET client_min_messages = debug1;
CREATE INDEX docs_idx ON docs USING chdb (
    body text_ops (tokenizer = 'ngrams', ngram_size = 2, support_phrase_search = true),
    title text_ops,
    tags text_array_ops,
    author columnar_ops,
    price,
    seen,
    flag columnar_ops
) WITH (vacuum_optimize_ratio = 0.5);
RESET client_min_messages;

\d docs_idx
SELECT reloptions FROM pg_class WHERE relname = 'docs_idx';

SET client_min_messages = debug1;
CREATE INDEX docs_title ON docs USING chdb (
    title text_ops (tokenizer = 'splitByString', tokenizer_arg = ' ,', preprocessor = 'caseFoldUTF8')
);
CREATE INDEX docs_regexp ON docs USING chdb (
    title text_ops (tokenizer = 'splitByRegexp', tokenizer_arg = '\s+''x', preprocessor = 'none')
);
CREATE INDEX docs_icu ON docs USING chdb (title text_ops (tokenizer = 'icu', tokenizer_arg = 'en'));
CREATE INDEX docs_html ON docs USING chdb (body text_ops (preprocessor = 'extractTextFromHTML'));
RESET client_min_messages;
DROP INDEX docs_regexp, docs_icu, docs_html;

-- Rejected options.
CREATE INDEX ON docs USING chdb (body text_ops (tokenizer = 'bogus'));
CREATE INDEX ON docs USING chdb (body text_ops (tokenizer = 'x); DROP TABLE docs; --'));
CREATE INDEX ON docs USING chdb (body text_ops (preprocessor = 'upper'));
CREATE INDEX ON docs USING chdb (body text_ops (ngram_size = 3));
CREATE INDEX ON docs USING chdb (body text_ops (ngram_size = 9, tokenizer = 'ngrams'));
CREATE INDEX ON docs USING chdb (body text_ops (tokenizer = 'icu'));
CREATE INDEX ON docs USING chdb (body text_ops (tokenizer = 'array', tokenizer_arg = 'x'));
CREATE INDEX ON docs USING chdb (tags text_array_ops (tokenizer = 'array'));
CREATE INDEX ON docs USING chdb (id columnar_ops (foo = 1));
CREATE INDEX ON docs USING chdb (body) WITH (nonsense = 1);
CREATE INDEX ON docs USING chdb (body) WITH (vacuum_optimize_ratio = 2);
CREATE INDEX ON docs USING chdb (id text_ops);
CREATE INDEX ON docs USING chdb (body) INCLUDE (title);
CREATE UNIQUE INDEX ON docs USING chdb (body);

-- Only permanent tables: an unlogged heap is reset by crash recovery while
-- its store is not, and a temporary one is rebuilt and dropped by backends
-- that need not have the library.
CREATE UNLOGGED TABLE udocs (body text);
CREATE INDEX ON udocs USING chdb (body);
CREATE TEMP TABLE tdocs (body text) ON COMMIT DELETE ROWS;
CREATE INDEX ON tdocs USING chdb (body);
DROP TABLE udocs, tdocs;

-- raw_preprocessor needs a superuser.
SET client_min_messages = debug1;
CREATE INDEX docs_raw ON docs USING chdb (
    body text_ops (raw_preprocessor = 'lowerUTF8(replaceAll(body, ''-'', '' ''))')
);
RESET client_min_messages;
DROP INDEX docs_raw;
CREATE ROLE chdb_search_user NOSUPERUSER;
GRANT CREATE ON SCHEMA public TO chdb_search_user;
SET ROLE chdb_search_user;
CREATE TABLE user_docs (body text);
CREATE INDEX ON user_docs USING chdb (body text_ops (raw_preprocessor = 'lower(body)'));
CREATE INDEX user_docs_idx ON user_docs USING chdb (body text_ops (tokenizer = 'splitByString'));
RESET ROLE;
DROP TABLE user_docs;
REVOKE CREATE ON SCHEMA public FROM chdb_search_user;
DROP ROLE chdb_search_user;

----------------------------------------------------------------------------
-- Planning: a chdb index scan, or a bitmap scan when forced
----------------------------------------------------------------------------
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'running shoes';
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@? 'running shoes' AND title @@= 'shoes';
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@~ 'running shoes';
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE tags @@@ 'sport';
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE author = 'ann' AND price >= 10 AND flag;
SET enable_indexscan = off;
SET enable_bitmapscan = on;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'running shoes';
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET enable_seqscan;

----------------------------------------------------------------------------
-- Generated SELECTs, with escaping of the search string
----------------------------------------------------------------------------
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET client_min_messages = debug1;
\o /dev/null
SELECT id FROM docs WHERE body @@@ 'running shoes';
SELECT id FROM docs WHERE body @@? 'it''s a \back\slash';
SELECT id FROM docs WHERE title @@= 'Shoes' AND tags @@@ 'sport' AND author >= 'a' AND price < 50.50;
SELECT id FROM docs WHERE seen > '2026-01-01 00:00:00+00' AND flag = false;
SELECT id FROM docs WHERE body @@@ NULL;
RESET client_min_messages;
SET enable_indexscan = off;
SET enable_bitmapscan = on;
SET client_min_messages = debug1;
SELECT id FROM docs WHERE body @@@ 'running shoes';
\o
RESET client_min_messages;
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET enable_seqscan;

-- Writes are in search_am_writes, rebuilds and drops in search_am_rebuild.

----------------------------------------------------------------------------
-- A session that first loads the library inside a DROP misses the drop
----------------------------------------------------------------------------
-- The object access hook that drops a store with its index is installed
-- when the library loads, which a fresh session does from index_drop's
-- relcache build, after the hook for that index would have fired. docs has
-- two indexes: the first drop loads the library and is missed, the second
-- is caught, so one `drop: idx_N` follows instead of two. The backend says
-- why, and the missed store waits for the worker's next start to be swept
-- (t/search_sweep.pl). (The GUC set before the load is a placeholder the
-- load takes over.)
\c
SET chdb_search.mask_oids = on;
SET client_min_messages = debug1;
DROP TABLE docs;
RESET client_min_messages;
DROP EXTENSION chdb_search;
SELECT count(*) FROM chdb.user_table;
-- The USAGE the script granted on the schema, which was not the extension's
-- to drop, stays with it.
SELECT has_schema_privilege('public', 'chdb', 'USAGE');
DROP TABLE chdb.user_table;

-- The worker stays connected to the database, which could not be dropped for
-- the next run, so stop it. There is none with the stub client.
DO $$
BEGIN
    PERFORM pg_terminate_backend(pid, 10000)
       FROM pg_stat_activity
      WHERE backend_type = 'chdb_search worker' AND datname = current_database();
END $$;
