-- chdb.score() through the custom scan with the stub worker client (make
-- CHDB_SEARCH_STUB=1): the plan and the statements the scan sends for the
-- weights, seen without a worker. The stub answers tokens() with
-- chdb_search_stub.tokens, a count() with the number of its ctids or the
-- frequency chdb_search_stub.frequencies gives a token, and the scan's
-- statement with the ctids, each scored with its own value. Runs only with
-- the stub (see the Makefile); search_score covers the worker.
\set VERBOSITY terse
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;
SET chdb_search.mask_oids = on;

CREATE TABLE docs (id int PRIMARY KEY, body text, price numeric(10, 2));
INSERT INTO docs VALUES
    (1, 'Running shoes', 49.90), (2, 'Walking boots', 89.00), (3, 'Trail shoes', 120.00);
CREATE INDEX docs_idx ON docs USING chdb (body, price);
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET chdb_search_stub.ctids = '1,2,3';
SET chdb_search_stub.tokens = 'running,shoes';
SET chdb_search_stub.frequencies = 'running:1,shoes:2';

----------------------------------------------------------------------------
-- The plan: the weights come from one tokens() and one count() per token
-- beside the table's count, asked once for the statement
----------------------------------------------------------------------------
SET client_min_messages = debug1;
EXPLAIN (COSTS OFF)
SELECT id, chdb.score(id) FROM docs WHERE body @@@ 'Running Shoes' ORDER BY chdb.score(id) DESC LIMIT 2;
RESET client_min_messages;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, chdb.score(id) FROM docs WHERE body @@@ 'Running Shoes' AND price < 100 AND id > 1;

----------------------------------------------------------------------------
-- The rows the store returns come with their scores
----------------------------------------------------------------------------
SELECT id, chdb.score(id) FROM docs WHERE body @@@ 'x' ORDER BY chdb.score(id) DESC LIMIT 2;
SELECT id, chdb.score(id) FROM docs WHERE body @@@ 'x' AND chdb.score(id) > 1 ORDER BY id;
-- A rescan with another needle asks the counts again only for new tokens:
-- the stub makes the same tokens of every needle, so none is asked twice.
SET client_min_messages = debug1;
SELECT q, d.id, d.score FROM (VALUES ('boots'), ('running')) v(q), LATERAL (SELECT id, chdb.score(id) FROM docs WHERE body @@@ v.q OFFSET 0) d ORDER BY 1, 2;
RESET client_min_messages;
-- Outside a custom scan the function raises.
SELECT chdb.score(id) FROM docs;
SELECT chdb.score(id) FROM docs WHERE body @@@ 'x' FOR UPDATE;

DROP TABLE docs;
DROP EXTENSION chdb_search;
