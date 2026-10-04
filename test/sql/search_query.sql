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
