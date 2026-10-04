-- Scans answered by the stub worker client (make CHDB_SEARCH_STUB=1), which
-- returns the ctids chdb_search_stub.ctids names: what the access method does
-- with the rows a search returns, seen without a worker. A ctid is packed as
-- (block << 16) | offset, so (0,1) is 1 and (1,1) is 65537. Runs only with
-- the stub (see the Makefile); search_e2e covers the worker.
\set VERBOSITY terse
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;
SET chdb_search.mask_oids = on;

CREATE TABLE docs (id int PRIMARY KEY, body text, loc point);
INSERT INTO docs VALUES (1, 'Running shoes', '(1,1)'), (2, 'Walking boots', '(2,2)'), (3, 'Trail shoes', '(3,3)');
CREATE INDEX docs_idx ON docs USING chdb (body);
SET enable_seqscan = off;

----------------------------------------------------------------------------
-- The rows a search returns are fetched from the heap by ctid
----------------------------------------------------------------------------
SET chdb_search_stub.ctids = '1,2';
SELECT id FROM docs WHERE body @@@ 'x' ORDER BY id;
-- The heap fetch decides visibility: a deleted row's ctid finds nothing.
DELETE FROM docs WHERE id = 2;
SELECT id FROM docs WHERE body @@@ 'x';
-- An offset past the page's line pointers is skipped by the heap fetch.
SET chdb_search_stub.ctids = '1,2,200';
SELECT id FROM docs WHERE body @@@ 'x';
-- A block at or past the heap's end, as a store left behind by a restore
-- holds, is skipped rather than read: the heap fetch would raise on it.
SET chdb_search_stub.ctids = '4294967296';
SELECT id FROM docs WHERE body @@@ 'x';
SET chdb_search_stub.ctids = '1,4294967296,65537';
SELECT id FROM docs WHERE body @@@ 'x';
-- No rows at all, as the stub answers by default.
RESET chdb_search_stub.ctids;
SELECT id FROM docs WHERE body @@@ 'x';

----------------------------------------------------------------------------
-- Answers a scan cannot read raise
----------------------------------------------------------------------------
SET chdb_search_stub.ctids = 'garbage';
SELECT id FROM docs WHERE body @@@ 'x';
SET chdb_search_stub.ctids = 'one,two';
SELECT id FROM docs WHERE body @@@ 'x';
RESET chdb_search_stub.ctids;
-- A worker lost mid-answer: the stream is closed and the error raised.
SET chdb_search_stub.ctids = '1';
SET chdb_search_stub.fail = on;
SELECT id FROM docs WHERE body @@@ 'x';
RESET chdb_search_stub.fail;
SELECT id FROM docs WHERE body @@@ 'x';

----------------------------------------------------------------------------
-- The fail-safe check: a store that does not match the metapage is refused
----------------------------------------------------------------------------
-- Before its first row a scan asks the store what it holds for the index's
-- generation and compares the last flush with the metapage's. The verdict
-- holds for that state of the metapage, so the scans above asked once and
-- these do not ask again: a flush of this session's own records the state
-- it leaves on both sides, which proves the store as well as asking would.
SET chdb_search_stub.ctids = '1';
SET client_min_messages = debug1;
SELECT id FROM docs WHERE body @@@ 'x';
INSERT INTO docs VALUES (4, 'Flushed', '(4,4)');
SELECT id FROM docs WHERE body @@@ 'x';
SELECT id FROM docs WHERE body @@@ 'x';
RESET client_min_messages;
-- With enough rows the planner takes the index for a search.
INSERT INTO docs SELECT 100 + i, 'filler', NULL FROM generate_series(1, 2000) i;
ANALYZE docs;
SET enable_seqscan = on;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'x';
-- A store whose last flush is not the index's, as a restore from backup or
-- pg_rewind leaves it, is refused before a commit flushes to it, which
-- would record the flush on both sides and pass the store as current: the
-- refusal aborts the commit, as it does a scan...
SET chdb_search_stub.meta = '1';
INSERT INTO docs VALUES (5, 'Another flush', '(5,5)');
SELECT id FROM docs WHERE body @@@ 'x';
-- ...and so is one that never saw this generation, as pg_upgrade leaves it.
SET chdb_search_stub.meta = 'none';
SELECT id FROM docs WHERE body @@@ 'x';
-- In skip mode the planner learns the same and takes another path, and a
-- scan forced on the index yields no rows rather than rows it cannot vouch
-- for.
SET chdb_search.unavailable_index = skip;
EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'x';
SELECT id FROM docs WHERE body @@@ 'Running' ORDER BY id;
SET enable_seqscan = off;
SELECT id FROM docs WHERE body @@@ 'x';
-- A commit keeps its rows from a store that cannot vouch for them either,
-- and moves the index on without it, so that the store stays refused, in
-- either mode, until a REINDEX rebuilds it from the heap the rows went to.
SET client_min_messages = debug1;
INSERT INTO docs VALUES (5, 'Kept from the store', '(5,5)');
RESET client_min_messages;
SELECT id FROM docs WHERE body @@@ 'x';
RESET chdb_search.unavailable_index;
SELECT id FROM docs WHERE body @@@ 'x';
-- The store agrees again.
RESET chdb_search_stub.meta;
SELECT id FROM docs WHERE body @@@ 'x';
DELETE FROM docs WHERE id > 3;

----------------------------------------------------------------------------
-- An ORDER BY scan returns the rows in the order the store gives them
----------------------------------------------------------------------------
ALTER OPERATOR FAMILY columnar_ops USING chdb ADD OPERATOR 1 <-> (point, point) FOR ORDER BY float_ops;
CREATE INDEX docs_loc ON docs USING chdb (loc);
SET chdb_search_stub.ctids = '3,1';
SELECT id FROM docs ORDER BY loc <-> '(0,0)' LIMIT 2;
RESET chdb_search_stub.ctids;
DROP INDEX docs_loc;
ALTER OPERATOR FAMILY columnar_ops USING chdb DROP OPERATOR 1 (point, point);

DROP TABLE docs;

----------------------------------------------------------------------------
-- An index-only scan the planner picks for a query needing no column
----------------------------------------------------------------------------
-- count(*) needs no column, so any index serves an index-only scan, this one
-- too though it returns none: it hands the executor an all-null index tuple
-- and the count comes from the store's rows on all-visible pages.
CREATE TABLE plain (body text);
INSERT INTO plain SELECT 'word' FROM generate_series(1, 5);
CREATE INDEX plain_idx ON plain USING chdb (body);
VACUUM plain;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM plain;
SET chdb_search_stub.ctids = '1,2,3';
SELECT count(*) FROM plain;
RESET chdb_search_stub.ctids;
RESET enable_seqscan;
DROP TABLE plain;
DROP EXTENSION chdb_search;
