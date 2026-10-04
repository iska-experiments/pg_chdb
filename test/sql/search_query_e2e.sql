-- The query language end to end: the searches below are answered by the
-- database's chdb_search worker from the index's ClickHouse table, and each
-- is compared with the Postgres fallback a sequential scan runs. The server
-- needs libchdb on its library path, for chdb_search_engine.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;
-- The index access method's own scans are under test: the custom scan,
-- which search_planner shows taking the same searches, is kept out of the
-- plans.
SET chdb_search.enable_custom_scan = off;

CREATE TABLE docs (
    id int PRIMARY KEY, body text, plain text GENERATED ALWAYS AS (body) STORED, tags text[]
);
INSERT INTO docs (id, body, tags) VALUES
    (1, 'Running shoes for runners', '{Sport,shoes}'),
    (2, 'Walking boots', '{outdoor}'),
    (3, 'Trail running shoes, fast and light', '{sport,trail}'),
    (4, 'a running b c shoes', '{}'),
    (5, 'a running b c d shoes', '{}'),
    (6, 'shoes for running', '{}'),
    (7, 'ÉCOLE in İstanbul', '{ÖL}'),
    (8, '50% off running_shoes <b>bold</b>', '{}'),
    (9, NULL, NULL);
-- body keeps the default preprocessor, lowerUTF8; plain has none.
CREATE INDEX docs_idx ON docs USING chdb (
    body, plain text_ops (preprocessor = 'none'), tags text_array_ops
);

-- The ids a predicate finds through the index and by sequential scan, and
-- whether the two agree.
CREATE FUNCTION pg_temp.both_ways(q text)
RETURNS TABLE (by_index int[], by_seqscan int[], agree bool)
LANGUAGE plpgsql AS $$
DECLARE a int[]; b int[];
BEGIN
    SET LOCAL enable_seqscan = off;
    SET LOCAL enable_indexscan = on;
    EXECUTE format('SELECT array_agg(id ORDER BY id) FROM docs WHERE %s', q) INTO a;
    SET LOCAL enable_seqscan = on;
    SET LOCAL enable_indexscan = off;
    EXECUTE format('SELECT array_agg(id ORDER BY id) FROM docs WHERE %s', q) INTO b;
    RETURN QUERY SELECT a, b, a IS NOT DISTINCT FROM b;
END $$;
-- (The index is what answers with sequential scans off.)
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@/ 'run+ing';
RESET enable_seqscan;

----------------------------------------------------------------------------
-- Regex and wildcard follow the index's preprocessor: lowercased text,
-- pattern matched without regard to case
----------------------------------------------------------------------------
SELECT v.q, b.* FROM (VALUES
    ($$body @@/ 'run+ing'$$),
    ($$body @@/ '^trail'$$),
    ($$body @@/ 'SHOES$'$$),
    ($$body @@/ 'r.nning sh'$$),
    ($$body @@/ 'école'$$),
    ($$body @@/ '\d+% off'$$),
    ($$body @@/ 'running\W+shoes'$$),
    ($$body @@% 'run%'$$),
    ($$body @@% '%SHOES'$$),
    ($$body @@% '%running\_shoes%'$$),
    ($$body @@% '50\% %'$$),
    ($$body @@% '%<b>%'$$),
    ($$body @@% 'a _unning%'$$),
    ($$body @@/ 'shoes' AND body @@% '%light'$$)
) v(q), LATERAL pg_temp.both_ways(v.q) b;
-- A column whose preprocessor keeps case is searched case-sensitively
-- through the index, while the fallback knows the default pipeline only:
-- such a column's patterns are meaningful through the index (which is also
-- what answered above).
SELECT v.q, b.* FROM (VALUES
    ($$plain @@/ 'running'$$),
    ($$plain @@/ 'Running'$$),
    ($$plain @@% 'Run%'$$)
) v(q), LATERAL pg_temp.both_ways(v.q) b;


----------------------------------------------------------------------------
-- chdb.query trees: and, or, not and boost over every kind of leaf
----------------------------------------------------------------------------
SELECT v.q, b.* FROM (VALUES
    ($$body @@@ chdb.match('boots trail')$$),
    ($$body @@@ chdb.match_all('running shoes')$$),
    ($$body @@@ (chdb.term('shoes') && !chdb.term('running'))$$),
    ($$body @@@ (chdb.term('boots') || chdb.phrase('for runners') || chdb.regex('^a '))$$),
    ($$body @@@ !(chdb.match('shoes') || chdb.match('boots'))$$),
    ($$body @@@ chdb.boost(chdb.wildcard('%shoes') && chdb.regex('\d'), 2)$$),
    ($$body @@@ chdb.all_of(chdb.term('a'), chdb.term('shoes'), !chdb.term('d'))$$),
    ($$body @@@ chdb.any_of(chdb.phrase('walking boots'), chdb.term('école'))$$),
    ($$body @@@ chdb.match('shoes') AND tags @@@ (chdb.term('SPORT') || chdb.term('outdoor'))$$),
    ($$tags @@@ (chdb.match('sport') && !chdb.match_all('trail'))$$),
    ($$tags @@@ !chdb.term('sport')$$)
) v(q), LATERAL pg_temp.both_ways(v.q) b;

----------------------------------------------------------------------------
-- Phrase slop, from token positions: 'a running b c shoes' (4) has two
-- tokens between, 'a running b c d shoes' (5) three, 'shoes for running' (6)
-- the words reversed
----------------------------------------------------------------------------
SELECT v.q, b.* FROM (VALUES
    ($$body @@@ chdb.phrase('running shoes')$$),
    ($$body @@@ chdb.phrase('running shoes', 1)$$),
    ($$body @@@ chdb.phrase('running shoes', 2)$$),
    ($$body @@@ chdb.phrase('running shoes', 3)$$),
    ($$body @@@ chdb.phrase('shoes running', 1)$$),
    ($$body @@@ chdb.phrase('a c shoes', 1)$$),
    ($$body @@@ chdb.phrase('a c shoes', 2)$$),
    ($$body @@@ chdb.phrase('a shoes', 3)$$),
    ($$body @@@ chdb.phrase('Running SHOES', 2)$$),
    ($$body @@@ chdb.phrase('running', 1)$$),
    ($$body @@@ chdb.phrase('nowhere shoes', 9)$$),
    ($$body @@@ (chdb.phrase('running shoes', 2) || chdb.term('boots'))$$),
    ($$body @@@ !chdb.phrase('running shoes', 3)$$)
) v(q), LATERAL pg_temp.both_ways(v.q) b;

-- A leaf naming another column is answered through the index alone.
SET enable_seqscan = off;
SELECT id FROM docs WHERE body @@@ (chdb.term('shoes') && chdb.in_column(chdb.term('trail'), 'tags')) ORDER BY id;
SELECT id FROM docs WHERE tags @@@ (chdb.term('outdoor') || chdb.in_column(chdb.regex('^a run'), 'body')) ORDER BY id;
RESET enable_seqscan;

DROP TABLE docs;
DROP EXTENSION chdb_search;

-- The worker stays connected to the database, which could not be dropped for
-- the next run, so stop it.
DO $$
BEGIN
    PERFORM pg_terminate_backend(pid, 10000)
       FROM pg_stat_activity
      WHERE backend_type = 'chdb_search worker' AND datname = current_database();
END $$;
