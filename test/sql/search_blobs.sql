-- The blobs behind a chdb index: its store tables keep their parts on
-- libchdb's callback object storage, one storage per index, whose blobs the
-- database's worker holds and chdb_search_blobs lists. The server needs
-- libchdb on its library path, for chdb_search_engine.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;

CREATE TABLE docs (id int PRIMARY KEY, body text);
INSERT INTO docs VALUES (1, 'Running shoes for runners'), (2, 'Walking boots');
CREATE INDEX docs_idx ON docs USING chdb (body);

-- The storage holds the parts of the store table and of the meta table
-- beside it, each file under a key libchdb chose, and plain_rewritable's
-- directory entries that name those keys. The keys are random, so the files
-- are shown by name; every blob was committed by the worker just now.
SELECT DISTINCT regexp_replace(key, '^[a-z]+/', '<key>/') AS file
  FROM chdb_search_blobs('docs_idx')
 WHERE key ~ '^[a-z]+/(checksums|columns|count|format_version)\.txt$'
 ORDER BY 1;
SELECT count(*) FILTER (WHERE key ~ '^__meta/[a-z]+/prefix\.path$') > 0 AS has_directory_entries,
       sum(size) > 0 AS has_bytes,
       bool_and(mtime BETWEEN now() - interval '1 hour' AND now() + interval '1 hour') AS committed_now
  FROM chdb_search_blobs('docs_idx');

-- A flush adds a part: its files appear under a new key.
SELECT count(DISTINCT split_part(key, '/', 1)) FILTER (WHERE key ~ '/count\.txt$') AS parts
  FROM chdb_search_blobs('docs_idx') \gset
INSERT INTO docs VALUES (3, 'Hiking boots');
SELECT count(DISTINCT split_part(key, '/', 1)) FILTER (WHERE key ~ '/count\.txt$') - :parts AS parts_added
  FROM chdb_search_blobs('docs_idx');
SET enable_seqscan = off;
SELECT id FROM docs WHERE body @@@ 'boots' ORDER BY id;
RESET enable_seqscan;

-- Each index has a storage of its own; a rebuild writes its new generation
-- into the same one, and VACUUM, dropping the generation that lost, removes
-- its blobs: the count of parts returns to what one generation holds.
CREATE INDEX docs_idx2 ON docs USING chdb (body);
SELECT count(*) > 0 AS second_storage FROM chdb_search_blobs('docs_idx2');
DROP INDEX docs_idx2;
SELECT count(DISTINCT split_part(key, '/', 1)) FILTER (WHERE key ~ '/count\.txt$') AS parts
  FROM chdb_search_blobs('docs_idx') \gset
REINDEX INDEX docs_idx;
SELECT count(DISTINCT split_part(key, '/', 1)) FILTER (WHERE key ~ '/count\.txt$') > :parts AS rebuilt_beside
  FROM chdb_search_blobs('docs_idx');
VACUUM docs;
SELECT count(DISTINCT split_part(key, '/', 1)) FILTER (WHERE key ~ '/count\.txt$') <= :parts AS old_generation_gone
  FROM chdb_search_blobs('docs_idx');
SET enable_seqscan = off;
SELECT id FROM docs WHERE body @@@ 'boots' ORDER BY id;
RESET enable_seqscan;

-- Only a chdb index has blobs, and only a role granted EXECUTE may list them.
SELECT chdb_search_blobs('docs_pkey');
CREATE ROLE search_blobs_nobody;
SET ROLE search_blobs_nobody;
SELECT chdb_search_blobs('docs_idx');
RESET ROLE;
DROP ROLE search_blobs_nobody;

DROP TABLE docs;
DROP EXTENSION chdb_search;
