-- chdb.score() through the custom scan: the store ranks the rows a text
-- search finds by an IDF-weighted overlap, idf(t) = ln((N - df(t) + 0.5) /
-- (df(t) + 0.5) + 1) summed over the query's tokens the row has, with N the
-- store's rows and df(t) the rows with the token. The corpus is small enough
-- to check by hand: of six rows, 'running' is in three, 'light', 'boots',
-- 'trail' and 'shoes' in two, 'socks' in one. Needs the worker, like
-- search_planner; search_stub_score covers the plans with the stub client.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;
SET chdb_search.mask_oids = on;

CREATE TABLE docs (id int PRIMARY KEY, body text, title text, tags text[], price numeric(10, 2));
INSERT INTO docs VALUES
    (1, 'Running shoes for runners', 'Shoes', '{Sport,shoes}', 49.90),
    (2, 'Walking boots', 'Boots', '{outdoor}', 89.00),
    (3, 'Trail running shoes, fast and light', 'Trail', '{sport,trail}', 120.00),
    (4, 'Running socks', 'Socks', '{sport}', 5.00),
    (5, NULL, NULL, NULL, NULL),
    (6, 'Light trail boots', 'Light', '{outdoor,trail}', 90.00);
CREATE INDEX docs_idx ON docs USING chdb (
    body, title text_ops (tokenizer = 'ngrams', ngram_size = 3), tags text_array_ops, price
);
SET enable_seqscan = off;
SET enable_bitmapscan = off;

-- The weight of a token by hand, from the table the store holds.
CREATE FUNCTION pg_temp.idf(tok text) RETURNS double precision LANGUAGE sql AS $$
    SELECT ln((n - df + 0.5) / (df + 0.5) + 1)
      FROM (SELECT count(*) AS n, count(*) FILTER (WHERE body @@= tok) AS df FROM docs) c
$$;
-- As search_planner's: EXPLAIN as Postgres 18 prints it.
CREATE FUNCTION pg_temp.explain(q text, actual bool DEFAULT false) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
    FOR line IN EXECUTE 'EXPLAIN (COSTS OFF' || CASE WHEN actual
        THEN ', ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF' ELSE '' END || ') ' || q LOOP
        line := regexp_replace(line, 'rows=(\d+) loops', 'rows=\1.00 loops', 'g');
        RETURN NEXT replace(line, 'InitPlan expr_', 'InitPlan ');
    END LOOP;
END $$;

----------------------------------------------------------------------------
-- The plan: the score is a column the store computes, with the weights of
-- the needle's tokens as the index counts them, and ORDER BY it DESC takes
-- the LIMIT along, ties broken by ctid
----------------------------------------------------------------------------
EXPLAIN (COSTS OFF)
SELECT id, chdb.score(id) FROM docs WHERE body @@? 'running light' ORDER BY chdb.score(id) DESC LIMIT 3;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, chdb.score(id) AS score FROM docs WHERE body @@? 'running light' AND chdb.score(id) > 1 ORDER BY id;

----------------------------------------------------------------------------
-- The ranking: a rare token outranks a common one, and a row with two
-- tokens outranks a row with either; equal scores come in ctid order
----------------------------------------------------------------------------
SELECT id, body, chdb.score(id) FROM docs WHERE body @@? 'running light' ORDER BY chdb.score(id) DESC LIMIT 3;
SELECT id, body, chdb.score(id) FROM docs WHERE body @@? 'running socks light' ORDER BY chdb.score(id) DESC;
SELECT id FROM docs WHERE body @@@ 'running' ORDER BY chdb.score(id) DESC LIMIT 2;
SELECT id, chdb.score(id) FROM docs WHERE body @@? 'running light' ORDER BY chdb.score(id) DESC OFFSET 1 LIMIT 2;
-- The store's weights are the formula's.
SELECT id, chdb.score(id) AS score,
       (pg_temp.idf('running') * (body @@= 'running')::int + pg_temp.idf('light') * (body @@= 'light')::int)::real AS by_hand
  FROM docs WHERE body @@? 'running light' ORDER BY id;
-- A score is per text column searched, summed; the second argument keeps one.
EXPLAIN (COSTS OFF)
SELECT id, chdb.score(id), chdb.score(id, 'title') FROM docs WHERE body @@? 'running light' AND title @@@ 'sho';
SELECT id, chdb.score(id), chdb.score(id, 'body'), chdb.score(id, 'title') FROM docs WHERE body @@? 'running light' AND title @@@ 'sho';
-- A text[] column: its elements are the tokens, lowercased as the index has them.
EXPLAIN (COSTS OFF) SELECT id, chdb.score(id) FROM docs WHERE tags @@= 'SPORT' ORDER BY chdb.score(id) DESC LIMIT 5;
SELECT id, chdb.score(id) FROM docs WHERE tags @@= 'SPORT' ORDER BY chdb.score(id) DESC LIMIT 5;
-- A WHERE on the score filters here, on the column the store computed.
SELECT id, chdb.score(id) FROM docs WHERE body @@? 'running light' AND chdb.score(id) > 1 ORDER BY id;

----------------------------------------------------------------------------
-- Wherever else the query puts the call, the scan computes it: an order
-- the store does not serve, a sort by two keys, an aggregate, a window, a
-- join above the scan, a rescan with another needle
----------------------------------------------------------------------------
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@? 'running light' ORDER BY chdb.score(id), id LIMIT 2;
SELECT id FROM docs WHERE body @@? 'running light' ORDER BY chdb.score(id), id LIMIT 2;
SELECT id FROM docs WHERE body @@? 'running light' ORDER BY chdb.score(id) DESC, id DESC LIMIT 2;
SELECT max(chdb.score(id)), count(*) FROM docs WHERE body @@? 'running light';
SELECT id, rank() OVER (ORDER BY chdb.score(id) DESC) FROM docs WHERE body @@? 'running light' ORDER BY 2, 1;
SELECT d.id, chdb.score(d.id), v.n FROM docs d JOIN (VALUES (1, 'x'), (3, 'y')) v(id, n) ON v.id = d.id WHERE d.body @@? 'running light' ORDER BY 1;
SELECT q, d.id, d.score FROM (VALUES ('boots'), ('running')) v(q), LATERAL (SELECT id, chdb.score(id) FROM docs WHERE body @@@ v.q OFFSET 0) d ORDER BY 1, 2;
SELECT *, chdb.score(id) FROM docs WHERE body @@@ 'light' ORDER BY id;
SELECT d, ctid, chdb.score(d.id) FROM docs d WHERE body @@@ 'light' ORDER BY id;
-- A NULL needle from a generic plan: no statement, no rows, no counts.
PREPARE q(text) AS SELECT id, chdb.score(id) FROM docs WHERE body @@@ $1;
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE q(NULL);
EXECUTE q(NULL);
EXECUTE q('running');
RESET plan_cache_mode;
DEALLOCATE q;

----------------------------------------------------------------------------
-- The custom scan is the only path a scoring query gets: the others would
-- evaluate the function in Postgres, where it raises
----------------------------------------------------------------------------
SET enable_seqscan = on;
SET enable_indexscan = off;
EXPLAIN (COSTS OFF) SELECT id, chdb.score(id) FROM docs WHERE body @@@ 'running';
RESET enable_indexscan;
SET enable_seqscan = off;
-- Outside one it raises: without a text search to score, in a FOR UPDATE
-- or a data-modifying statement, which recheck rows in Postgres, bound to
-- no table, naming a column the index has no text index on, or with the
-- custom scan turned off.
SELECT chdb.score(id) FROM docs;
SELECT chdb.score(id) FROM docs WHERE price < 100;
SELECT chdb.score(id) FROM docs WHERE body @@@ 'running' FOR UPDATE;
UPDATE docs SET price = chdb.score(id) WHERE body @@@ 'running';
SELECT chdb.score(1);
SELECT chdb.score(id, 'price') FROM docs WHERE body @@@ 'running';
SELECT chdb.score(id, 'nope') FROM docs WHERE body @@@ 'running';
SET chdb_search.enable_custom_scan = off;
SELECT chdb.score(id) FROM docs WHERE body @@@ 'running';
RESET chdb_search.enable_custom_scan;

----------------------------------------------------------------------------
-- A deleted row the store still holds does not shorten the ranking: the
-- scan asks again with the same weights
----------------------------------------------------------------------------
DELETE FROM docs WHERE id = 3;
SELECT * FROM pg_temp.explain($$SELECT id FROM docs WHERE body @@? 'running light' ORDER BY chdb.score(id) DESC LIMIT 2$$, true);
SELECT id, chdb.score(id) FROM docs WHERE body @@? 'running light' ORDER BY chdb.score(id) DESC LIMIT 2;

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
