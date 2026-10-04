-- Run once the chdb access method (search-am) is merged, in a scratch db:
--   DROP EXTENSION IF EXISTS chdb_vector;  -- recreate so the opclass DO block sees the AM
--   psql -h 127.0.0.1 -p 54322 -U postgres -f dev/vector_opclass_check.sql
-- Promote to test/sql/ with an expected file when it passes.
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS chdb_search;
DROP EXTENSION IF EXISTS chdb_vector;
CREATE EXTENSION chdb_vector;

SELECT opcname, amopstrategy, amoppurpose
  FROM pg_opclass c JOIN pg_am a ON a.oid = c.opcmethod
  JOIN pg_amop o ON o.amopfamily = c.opcfamily
 WHERE a.amname = 'chdb' AND opcname LIKE 'vector\_%' ORDER BY 1;

CREATE TABLE items (id bigint PRIMARY KEY, body text, embedding vector(3));
INSERT INTO items VALUES (1, 'running shoes', '[1,0,0]'), (2, 'sandals', '[0,1,0]');
CREATE INDEX ON items USING chdb (body, embedding vector_cosine_ops);
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM items ORDER BY embedding <=> '[1,0,0]' LIMIT 1;
SELECT id FROM items ORDER BY embedding <=> '[1,0,0]' LIMIT 1;
