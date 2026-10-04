-- Tests of the statements the chdb index access method generates, logged at
-- DEBUG1. The output is the same with the worker and with the stub client
-- (make CHDB_SEARCH_STUB=1), which accepts everything and returns no rows: rows
-- found through the index are discarded here and checked by search_e2e.
\set VERBOSITY terse
-- The extension installs into schema chdb, which it creates or, as here,
-- finds already in use, and which DROP EXTENSION then leaves alone. (The
-- schema may already exist from an earlier test in the same database; the
-- notice for that is beside the point and silenced.)
SET client_min_messages = warning;
CREATE SCHEMA IF NOT EXISTS chdb;
CREATE TABLE chdb.user_table (x int);
CREATE EXTENSION chdb_search;
RESET client_min_messages;
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
-- Lowercased by Unicode, as lowerUTF8 does, whatever the cluster's ctype.
SELECT 'ÉCOLE' @@@ 'école', 'ÖL' @@= 'öl', chdb.has_token('ÉCOLE', 'école'), 'ÉCOLE' @@~ 'école',
       ARRAY['ÉCOLE'] @@@ 'école';
-- has_token judges the needle as written: a separator in it raises, an
-- empty one matches nothing, and lowercasing may change its length.
SELECT chdb.has_token('İzmir ve İstanbul', 'İzmir');
SELECT chdb.has_token('x ⱥ y', 'Ⱥ ');
SELECT chdb.has_token('x', '');

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
    author text COLLATE "C",
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

-- Column names are quoted whatever they are: a keyword stays a name, and
-- inf or nan stay columns rather than float literals.
CREATE TABLE kw ("index" text, "constraint" text COLLATE "C", inf float8, nan float8);
SET client_min_messages = debug1;
CREATE INDEX kw_idx ON kw USING chdb ("index", "constraint" columnar_ops, inf, nan);
SET enable_seqscan = off;
\o /dev/null
SELECT * FROM kw WHERE inf = 1 AND nan < 2 AND "constraint" = 'x';
\o
RESET enable_seqscan;
RESET client_min_messages;
DROP TABLE kw;

-- text[] defaults to text_array_ops. Any other type without a class of its
-- own takes columnar_ops, which refuses a type its family has no operators
-- for: the column would be stored at every write and serve no query. The
-- integer and float families have their cross-type pairs, so i2 = 1 is
-- pushed down as written.
CREATE TABLE more (n int[], j jsonb, i2 int2, tg text[]);
INSERT INTO more VALUES ('{1}', '{}', 1, '{sport}');
CREATE INDEX more_tg ON more USING chdb (tg);
SELECT c.opcname FROM pg_index i JOIN pg_opclass c ON c.oid = i.indclass[0]
 WHERE i.indexrelid = 'more_tg'::regclass;
CREATE INDEX ON more USING chdb (n);
CREATE INDEX ON more USING chdb (j);
CREATE INDEX more_i2 ON more USING chdb (i2);
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT * FROM more WHERE tg @@@ 'sport';
EXPLAIN (COSTS OFF) SELECT * FROM more WHERE i2 = 1 AND i2 < 20000000000;
RESET enable_seqscan;
DROP TABLE more;

-- A column's kind comes from its class's options support function, not the
-- family's name: a class without text options, as another extension might
-- declare, stores a plain column, and its operators are still rendered by
-- strategy number (text searches are 1..4, comparisons 11..15).
CREATE OPERATOR CLASS my_text FOR TYPE text USING chdb AS
    OPERATOR 1 @@@ (text, text),
    FUNCTION 1 (text) chdb_search_no_options(internal);
SELECT amvalidate(oid) FROM pg_opclass WHERE opcname = 'my_text';
CREATE TABLE mine (body text COLLATE "C");
SET client_min_messages = debug1;
CREATE INDEX mine_idx ON mine USING chdb (body my_text);
SET enable_seqscan = off;
\o /dev/null
SELECT body FROM mine WHERE body @@@ 'running shoes';
\o
RESET enable_seqscan;
RESET client_min_messages;
DROP TABLE mine;
DROP OPERATOR CLASS my_text USING chdb;
-- Renaming a family does not change how its indexes are searched.
ALTER OPERATOR FAMILY text_ops USING chdb RENAME TO text_ops_v2;
SET client_min_messages = debug1;
SET enable_seqscan = off;
\o /dev/null
SELECT id FROM docs WHERE body @@@ 'running shoes';
\o
RESET enable_seqscan;
RESET client_min_messages;
ALTER OPERATOR FAMILY text_ops_v2 USING chdb RENAME TO text_ops;

-- A text column filtered through the index needs a bytewise collation:
-- ClickHouse compares strings bytewise and the scan does not recheck. Only
-- C and POSIX count as bytewise, as for btree's text_pattern_ops, so the
-- negative case takes any other collation initdb imported: libc's C.utf8
-- (glibc 2.35 and later; it sorts by code point too) where there is one,
-- else the first other libc or ICU collation, whatever the platform has.
SELECT quote_ident(collname) AS noncoll FROM pg_collation
 WHERE (collprovider = 'c' AND collcollate NOT IN ('C', 'POSIX') OR collprovider = 'i')
   AND collencoding IN (-1, pg_char_to_encoding('UTF8'))
 ORDER BY collname <> 'C.utf8', collprovider, collname LIMIT 1 \gset
CREATE TABLE coll (author text COLLATE :noncoll, c_author text COLLATE "C");
CREATE INDEX ON coll USING chdb (author columnar_ops);
CREATE INDEX ON coll USING chdb ((author COLLATE "C") columnar_ops);
CREATE INDEX ON coll USING chdb (c_author columnar_ops);
DROP TABLE coll;

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
-- chdb_search.unavailable_index: the fail-safe GUC (its default and values)
----------------------------------------------------------------------------
SHOW chdb_search.unavailable_index;
SET chdb_search.unavailable_index = skip;
SET chdb_search.unavailable_index = error;
SET chdb_search.unavailable_index = bogus;
-- Not in recovery, so the store is available and a scan is unaffected either way.
SET enable_seqscan = off;
\o /dev/null
SET chdb_search.unavailable_index = skip;
SELECT id FROM docs WHERE body @@@ 'running shoes';
SET chdb_search.unavailable_index = error;
SELECT id FROM docs WHERE body @@@ 'running shoes';
\o
RESET chdb_search.unavailable_index;
RESET enable_seqscan;

----------------------------------------------------------------------------
-- Planning: a chdb index scan, and never a bitmap scan
----------------------------------------------------------------------------
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'running shoes';
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@? 'running shoes' AND title @@= 'shoes';
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@~ 'running shoes';
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE tags @@@ 'sport';
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE author = 'ann' AND price >= 10 AND flag;
-- The access method has no amgetbitmap: a lossy bitmap's recheck would run
-- the Postgres fallbacks, which know the default tokenizer only. With the
-- sequential and index scans both disabled a bitmap path would win, yet the
-- planner still has only the heap. 18 marks that plan `Disabled: true` where
-- 17 adds disable_cost; the wrapper drops the line so both read the same.
CREATE FUNCTION pg_temp.plan_lines(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
    FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
        IF line !~ '^\s*Disabled: ' THEN RETURN NEXT line; END IF;
    END LOOP;
END $$;
SET enable_indexscan = off;
SET enable_bitmapscan = on;
SELECT * FROM pg_temp.plan_lines($$SELECT id FROM docs WHERE body @@@ 'running shoes'$$);
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET enable_seqscan;

-- The statements scans generate are in search_am_scan, writes in
-- search_am_writes, rebuilds and drops in search_am_rebuild.

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
