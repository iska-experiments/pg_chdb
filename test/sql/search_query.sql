-- The query language of chdb indexes: the pattern operators, the chdb.query
-- type with its builders, and the statements they make the access method
-- generate, logged at DEBUG1. The output is the same with the worker and
-- with the stub client (see search_am.sql): the rows a scan returns are
-- discarded here and checked by search_query_e2e.
\set VERBOSITY terse
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;
SET chdb_search.mask_oids = on;
-- The index access method's own scans are under test: the custom scan,
-- which search_planner shows taking the same searches, is kept out of the
-- plans.
SET chdb_search.enable_custom_scan = off;

----------------------------------------------------------------------------
-- Regex and wildcard: the Postgres fallback of the default preprocessor
----------------------------------------------------------------------------
-- The text is lowercased and the pattern matched without regard to case.
SELECT 'Running shoes' @@/ 'run+ing', 'Running shoes' @@/ '^RUN', 'Running shoes' @@/ 'shoes$',
       'Running shoes' @@/ 'r.nning sh', 'Running shoes' @@/ 'boots', 'ÉCOLE' @@/ 'école';
SELECT chdb.regex('a b', '\Wb'), chdb.regex('ab', 'a\Wb'), chdb.regex('x', '');
SELECT 'x' @@/ '(';
SELECT 'Running shoes' @@% 'run%', 'Running shoes' @@% '%SHOES', 'Running shoes' @@% 'run_ing%',
       'Running shoes' @@% 'run%g', 'Running shoes' @@% 'shoes', 'Running shoes' @@% '%';
-- A backslash escapes %, _ and itself; _ is one character, not one byte.
SELECT chdb.wildcard('50% off', '50\% %'), chdb.wildcard('50x off', '50\% %'),
       chdb.wildcard('a_b', 'a\_b'), chdb.wildcard('axb', 'a\_b'), chdb.wildcard('a\b', 'a\\b'),
       chdb.wildcard('é', '_'), chdb.wildcard('é', '__'), chdb.wildcard('', '');
SELECT NULL::text @@/ 'x', 'x' @@% NULL;

----------------------------------------------------------------------------
-- chdb.query: the builders and the readable form
----------------------------------------------------------------------------
SELECT chdb.match('running shoes'), chdb.match_all('running shoes'), chdb.term('shoes'),
       chdb.phrase('running shoes'), chdb.phrase('running shoes', 2), chdb.regex('run+'),
       chdb.wildcard('run%');
-- && and || flatten a chain into one group; ! negates; boost carries a weight.
SELECT chdb.term('a') && chdb.term('b') && chdb.term('c'),
       chdb.term('a') || (chdb.term('b') || chdb.term('c')),
       (chdb.term('a') && chdb.term('b')) || chdb.term('c'),
       !chdb.term('a'), !(!chdb.term('a')),
       chdb.boost(chdb.term('a'), 2.5), chdb.boost(chdb.match('a b') && chdb.term('c'), 0.5);
SELECT chdb.all_of(chdb.term('a'), chdb.term('b') && chdb.term('c')),
       chdb.any_of(chdb.term('a')), chdb.none_of(chdb.phrase('a b', 1)),
       chdb.in_column(chdb.term('a') || chdb.in_column(chdb.term('b'), 'other'), 'title');
-- A needle keeps its quotes, backslashes and newlines through the form.
SELECT chdb.term(E'it''s a \\ back\nslash');
SELECT chdb.term(E'it''s a \\ back\nslash')::text::chdb.query::text = chdb.term(E'it''s a \\ back\nslash')::text;
-- The form reads back, whitespace and all.
SELECT 'and( match_all( ''running shoes'' ) ,not(phrase(''on sale'',1)), boost(term(''new''), 2))'::chdb.query;
SELECT 'in_column(or(term(''a''), wildcard(''b%'')), ''title'')'::chdb.query;
-- Rejected forms and arguments.
SELECT 'bogus(''x'')'::chdb.query;
SELECT 'term(''x'::chdb.query;
SELECT 'term(''x'') extra'::chdb.query;
SELECT 'boost(term(''x''), two)'::chdb.query;
SELECT 'not(term(''x''), term(''y''))'::chdb.query;
SELECT 'phrase(''x'', -1)'::chdb.query;
SELECT chdb.phrase('x', -1);
SELECT chdb.all_of(VARIADIC ARRAY[]::chdb.query[]);
SELECT chdb.any_of(chdb.term('a'), NULL);

----------------------------------------------------------------------------
-- The Postgres fallback evaluates the tree with the fallback of each leaf
----------------------------------------------------------------------------
SELECT 'Running shoes for runners' @@@ chdb.match('boots shoes'),
       'Running shoes for runners' @@@ chdb.match_all('boots shoes'),
       'Running shoes for runners' @@@ (chdb.term('shoes') && !chdb.term('boots')),
       'Running shoes for runners' @@@ (chdb.term('boots') || chdb.phrase('for runners')),
       'Running shoes for runners' @@@ chdb.boost(chdb.regex('^run') && chdb.wildcard('%runners'), 3),
       'Running shoes for runners' @@@ !chdb.match('running'),
       NULL::text @@@ chdb.term('x');
SELECT ARRAY['Sport', 'shoes'] @@@ chdb.any_of(chdb.term('SHOES'), chdb.term('boots')),
       ARRAY['Sport', 'shoes'] @@@ (chdb.match('sport') && !chdb.match_all('trail'));
-- A NULL array reads as the empty array the index stores for it.
SELECT NULL::text[] @@@ chdb.term('x'), NULL::text[] @@@ !chdb.term('x'), '{}'::text[] @@@ !chdb.term('x'),
       NULL::text[] @@@ NULL;
SELECT ARRAY['a'] @@@ chdb.phrase('a b');
SELECT ARRAY['a'] @@@ chdb.regex('a');
SELECT 'x' @@@ chdb.in_column(chdb.term('x'), 'title');
SELECT 'x' @@@ chdb.term('two words');

----------------------------------------------------------------------------
-- The operator class carries the new strategies
----------------------------------------------------------------------------
SELECT amopstrategy, amopopr::regoperator
  FROM pg_amop
 WHERE amopfamily = (SELECT oid FROM pg_opfamily WHERE opfname = 'text_ops'
                        AND opfmethod = (SELECT oid FROM pg_am WHERE amname = 'chdb'))
 ORDER BY amopstrategy;
SELECT opcname, amvalidate(oid)
  FROM pg_opclass
 WHERE opcmethod = (SELECT oid FROM pg_am WHERE amname = 'chdb')
 ORDER BY opcname;

----------------------------------------------------------------------------
-- The statements scans generate: the pattern follows the preprocessor
----------------------------------------------------------------------------
CREATE TABLE docs (id int PRIMARY KEY, body text, html text, plain text, raw text, tags text[]);
INSERT INTO docs VALUES (1, 'Running shoes', '<b>Bold</b>', 'Plain', 'Hyphen-ated', '{sport}');
CREATE INDEX docs_idx ON docs USING chdb (
    body,
    html text_ops (preprocessor = 'extractTextFromHTML'),
    plain text_ops (preprocessor = 'none'),
    raw text_ops (raw_preprocessor = 'lowerUTF8(replaceAll(raw, ''-'', '' ''))'),
    tags text_array_ops
);
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@/ 'run+ing' AND body @@% 'Run%';
SET client_min_messages = debug1;
\o /dev/null
SELECT id FROM docs WHERE body @@/ 'run+ing';
SELECT id FROM docs WHERE body @@% 'Run%';
SELECT id FROM docs WHERE html @@/ 'bold' AND plain @@% 'Pla%';
SELECT id FROM docs WHERE raw @@/ 'hyphen ated' AND raw @@% 'hyphen%';
SELECT id FROM docs WHERE body @@/ E'it''s \\d+' AND body @@% E'50\\% ''off''%';
\o
RESET client_min_messages;
-- An array column has no pattern operators.
SELECT id FROM docs WHERE tags @@/ 'x';
SELECT id FROM docs WHERE tags @@% 'x';

----------------------------------------------------------------------------
-- A chdb.query is one term of the WHERE, with each leaf as its operator
-- renders it; the planner folds the builders to a constant
----------------------------------------------------------------------------
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ (chdb.match('running') && !chdb.term('boots'));
SET client_min_messages = debug1;
\o /dev/null
SELECT id FROM docs WHERE body @@@ chdb.match('running shoes');
SELECT id FROM docs WHERE body @@@ (chdb.match_all('running shoes') && !chdb.term('boots'));
SELECT id FROM docs WHERE body @@@ (chdb.term('a') || chdb.phrase('b c') || chdb.boost(chdb.regex('d+'), 2));
-- A text column with a NOT in its tree is guarded against NULL, which the
-- strict operator never matches while ClickHouse's NOT false would.
SELECT id FROM docs WHERE body @@@ !(chdb.wildcard('a%') && chdb.term('b')) AND body @@= 'c';
SELECT id FROM docs WHERE tags @@@ !chdb.term('sport');
SELECT id FROM docs WHERE tags @@@ chdb.any_of(chdb.term('sport'), chdb.match_all('trail'));
-- A leaf naming another column of the index searches that column.
SELECT id FROM docs WHERE body @@@ (chdb.term('a') && chdb.in_column(chdb.term('b'), 'html') && chdb.in_column(chdb.match('c'), 'tags'));
\o
RESET client_min_messages;
SELECT id FROM docs WHERE body @@@ chdb.in_column(chdb.term('a'), 'nope');
SELECT id FROM docs WHERE body @@@ chdb.in_column(chdb.term('a'), 'id');
SELECT id FROM docs WHERE tags @@@ chdb.phrase('a b');
RESET enable_seqscan;

DROP TABLE docs;
DROP EXTENSION chdb_search;

-- The worker stays connected to the database, which could not be dropped for
-- the next run, so stop it. There is none with the stub client.
DO $$
BEGIN
    PERFORM pg_terminate_backend(pid, 10000)
       FROM pg_stat_activity
      WHERE backend_type = 'chdb_search worker' AND datname = current_database();
END $$;
