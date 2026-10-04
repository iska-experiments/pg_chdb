-- VACUUM through the stub worker client (make CHDB_SEARCH_STUB=1): the ctids
-- chdb_search_stub.ctids names stand for the store's rows, so what VACUUM
-- deletes from the store, and when it merges the store's parts, is seen
-- without a worker. Runs only with the stub (see the Makefile); search_e2e
-- covers the worker.
\set VERBOSITY terse
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;
SET chdb_search.mask_oids = on;

CREATE TABLE docs (id int PRIMARY KEY, body text);
INSERT INTO docs VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd');
CREATE INDEX docs_idx ON docs USING chdb (body) WITH (vacuum_optimize_ratio = 0.5);

-- The store holds ctids 1 to 4 and one heap row is dead: VACUUM deletes its
-- ctid and, with a quarter of the rows dead, does not merge.
SET chdb_search_stub.ctids = '1,2,3,4';
DELETE FROM docs WHERE id = 2;
SET client_min_messages = debug1;
VACUUM docs;
RESET client_min_messages;
-- Past the index's ratio, the parts are merged after the delete.
ALTER INDEX docs_idx SET (vacuum_optimize_ratio = 0.1);
DELETE FROM docs WHERE id = 1;
SET client_min_messages = debug1;
VACUUM docs;
RESET client_min_messages;
-- Without the reloption the GUC decides; ANALYZE alone asks the store nothing.
ALTER INDEX docs_idx RESET (vacuum_optimize_ratio);
SET chdb_search.vacuum_optimize_ratio = 0;
SET client_min_messages = debug1;
ANALYZE docs;
-- Nothing dead: no delete, no merge, only the sweep of other generations.
RESET chdb_search_stub.ctids;
VACUUM docs;
RESET client_min_messages;
RESET chdb_search.vacuum_optimize_ratio;

DROP TABLE docs;
DROP EXTENSION chdb_search;
