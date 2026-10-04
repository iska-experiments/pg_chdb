-- Tests of the chdb index access method against the stub worker client
-- (make CHDB_SEARCH_STUB=1): every statement the access method generates is
-- logged at DEBUG1, and the stub accepts it and returns no rows.
\set VERBOSITY terse
CREATE EXTENSION chdb_search;
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
CREATE INDEX ON docs USING chdb (body, (title || 'x'), (title || 'y'));
CREATE INDEX ON docs USING chdb (id text_ops);
CREATE INDEX ON docs USING chdb (body) INCLUDE (title);
CREATE UNIQUE INDEX ON docs USING chdb (body);

-- raw_preprocessor needs a superuser.
SET client_min_messages = debug1;
CREATE INDEX docs_raw ON docs USING chdb (
    body text_ops (raw_preprocessor = 'lowerUTF8(replaceAll(body, ''-'', '' ''))')
);
RESET client_min_messages;
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
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'running shoes';
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@? 'running shoes' AND title @@= 'shoes';
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@~ 'running shoes';
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE tags @@@ 'sport';
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE author = 'ann' AND price >= 10 AND flag;
SET enable_indexscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'running shoes';
RESET enable_indexscan;
RESET enable_seqscan;

----------------------------------------------------------------------------
-- Generated SELECTs, with escaping of the search string
----------------------------------------------------------------------------
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET client_min_messages = debug1;
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
RESET client_min_messages;
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET enable_seqscan;

----------------------------------------------------------------------------
-- Writes: buffered, flushed at COMMIT, dropped at ROLLBACK
----------------------------------------------------------------------------
SET client_min_messages = debug1;
BEGIN;
INSERT INTO docs VALUES (3, 'committed row', 'T', '{a}', 'cy', 1, now(), true);
\echo -- nothing is sent before COMMIT
COMMIT;
\echo -- rolled back
BEGIN;
INSERT INTO docs VALUES (4, 'rolled back row', 'T', '{a}', 'dee', 1, now(), true);
ROLLBACK;
\echo -- savepoints
BEGIN;
INSERT INTO docs VALUES (5, 'kept', 'T', '{a}', 'eve', 1, now(), true);
SAVEPOINT s1;
INSERT INTO docs VALUES (6, 'dropped by rollback to', 'T', '{a}', 'fay', 1, now(), true);
ROLLBACK TO s1;
SAVEPOINT s2;
INSERT INTO docs VALUES (7, 'released', 'T', '{a}', 'gus', 1, now(), true);
RELEASE s2;
COMMIT;
\echo -- update and delete
UPDATE docs SET body = 'changed' WHERE id = 1;
DELETE FROM docs WHERE id = 2;
\echo -- rows with NULLs
INSERT INTO docs (id) VALUES (8);
\echo -- a failing statement
INSERT INTO docs VALUES (1, 'duplicate key');
RESET client_min_messages;

----------------------------------------------------------------------------
-- VACUUM
----------------------------------------------------------------------------
SET client_min_messages = debug1;
VACUUM docs;
RESET client_min_messages;

----------------------------------------------------------------------------
-- REINDEX, TRUNCATE, DROP INDEX, DROP TABLE
----------------------------------------------------------------------------
SET client_min_messages = debug1;
REINDEX INDEX docs_title;
TRUNCATE docs;
DROP INDEX docs_title;
BEGIN;
DROP INDEX docs_icu;
ROLLBACK;
\echo -- a rolled-back CREATE INDEX drops its store
BEGIN;
CREATE INDEX docs_rolled ON docs USING chdb (title text_ops);
ROLLBACK;
DROP TABLE docs;
RESET client_min_messages;

----------------------------------------------------------------------------
-- Reserved and duplicate column names
----------------------------------------------------------------------------
CREATE TABLE reserved (ctid2 text, "xmin" text);
CREATE INDEX ON reserved USING chdb ("xmin" text_ops);
DROP TABLE reserved;

DROP EXTENSION chdb_search;
