-- The chdb index access method end to end: the searches below are answered by
-- the database's chdb_search worker from the index's ClickHouse table. The
-- server needs libchdb on its library path or chdb_search.libchdb_path.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION chdb_search;
SET chdb_search.mask_oids = on;

CREATE TABLE prod (id int PRIMARY KEY, body text, tags text[], price numeric(10, 2));
INSERT INTO prod VALUES
    (1, 'Running shoes for runners', '{Sport,shoes}', 49.90),
    (2, 'Walking boots', '{outdoor}', 89.00),
    (3, 'Trail running shoes, fast and light', '{sport,trail}', 120.00);
CREATE INDEX prod_idx ON prod USING chdb (body, tags text_array_ops, price columnar_ops);
BEGIN;
INSERT INTO prod VALUES
    (4, E'It''s a \\back\\slash\nand a new line', E'{"o''clock","c:\\\\dir"}', 5.00),
    (5, NULL, NULL, NULL);
COMMIT;

-- The index's ClickHouse table, read through the worker. ctid is the heap TID
-- packed as (block << 16) | offset; a NULL array is stored empty.
SELECT 'prod_idx'::regclass::oid AS idx \gset
CREATE FUNCTION pg_temp.store(oid)
RETURNS TABLE (ctid bigint, body text, tags text[], price numeric)
LANGUAGE sql AS $$
    SELECT * FROM chdb_search_query(format(
        'SELECT toInt64(ctid), body, tags, price FROM idx_%s.t ORDER BY ctid', $1
    )) AS (ctid bigint, body text, tags text[], price numeric)
$$;
SELECT * FROM pg_temp.store(:idx) ORDER BY ctid;

-- Every heap row's TID packs to the store row holding its values.
SELECT p.id, p.ctid, s.ctid AS packed,
       (s.body, s.price) IS NOT DISTINCT FROM (p.body, p.price) AS same
  FROM prod p
  LEFT JOIN pg_temp.store(:idx) s
    ON s.ctid = (p.ctid::text::point)[0]::bigint * 65536 + (p.ctid::text::point)[1]::bigint
 ORDER BY p.id;

----------------------------------------------------------------------------
-- Searches through the index
----------------------------------------------------------------------------
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM prod WHERE body @@@ 'running shoes' ORDER BY id;
SELECT id FROM prod WHERE body @@@ 'running shoes' ORDER BY id;
SELECT id FROM prod WHERE body @@@ 'RUNNING Shoes' ORDER BY id;
SELECT id FROM prod WHERE body @@? 'boots trail' ORDER BY id;
SELECT id FROM prod WHERE body @@~ 'running shoes' ORDER BY id;
SELECT id FROM prod WHERE body @@~ 'shoes running' ORDER BY id;
SELECT id FROM prod WHERE body @@= 'boots' ORDER BY id;
SELECT id FROM prod WHERE tags @@@ 'SPORT' ORDER BY id;
SELECT id FROM prod WHERE tags @@= 'trail' ORDER BY id;
SELECT id FROM prod WHERE tags @@? 'outdoor' ORDER BY id;
SELECT id FROM prod WHERE body @@@ NULL ORDER BY id;

-- A columnar filter with a text predicate, both sent to ClickHouse.
EXPLAIN (COSTS OFF) SELECT id FROM prod WHERE body @@@ 'shoes' AND price < 100 ORDER BY id;
SET client_min_messages = debug1;
SELECT id FROM prod WHERE body @@@ 'shoes' AND price < 100 ORDER BY id;
SET client_min_messages = warning;
SELECT id FROM prod WHERE body @@@ 'shoes' AND price >= 49.90 AND price <= 120 ORDER BY id;
SELECT id FROM prod WHERE price = 89 ORDER BY id;

-- A bitmap scan.
SET enable_indexscan = off;
SET enable_bitmapscan = on;
EXPLAIN (COSTS OFF) SELECT id FROM prod WHERE body @@? 'shoes boots' ORDER BY id;
SELECT id FROM prod WHERE body @@? 'shoes boots' ORDER BY id;
RESET enable_indexscan;
SET enable_bitmapscan = off;

-- The tokenizer, run by the worker.
SELECT chdb.tokens('Running Shoes, fast!');
SELECT chdb.tokens(E'It''s a \\back\\slash\nnew line');

----------------------------------------------------------------------------
-- String literals reach ClickHouse intact: quotes, backslashes, newlines
----------------------------------------------------------------------------
SELECT id FROM prod WHERE body @@@ E'it''s' ORDER BY id;
SELECT id FROM prod WHERE body @@@ E'back\\slash' ORDER BY id;
SELECT id FROM prod WHERE body @@~ E'slash\nand' ORDER BY id;
SELECT id FROM prod WHERE tags @@= E'o''clock' ORDER BY id;
SELECT id FROM prod WHERE tags @@= E'c:\\dir' ORDER BY id;
-- A needle cannot end the literal: this is five tokens, which no row has.
SELECT id FROM prod WHERE body @@@ $$shoes') OR hasAnyTokens(body, 'x$$ ORDER BY id;
SELECT id FROM prod WHERE body @@@ E'shoes\\'' OR 1 = 1 OR ''' ORDER BY id;

-- The answers come from the store: a row deleted there behind Postgres's back
-- is no longer found through the index, though the heap still has it.
SELECT chdb_search_exec(format('DELETE FROM idx_%s.t WHERE ctid = 3', :idx));
SELECT id FROM prod WHERE body @@@ 'running shoes' ORDER BY id;
SET enable_seqscan = on;
SET enable_indexscan = off;
SELECT id FROM prod WHERE body @@@ 'running shoes' ORDER BY id;
RESET enable_indexscan;
SET enable_seqscan = off;
-- A search ClickHouse cannot run raises, rather than finding nothing.
SELECT chdb_search_exec(format('DROP TABLE idx_%s.t', :idx));
SELECT id FROM prod WHERE body @@@ 'running shoes' ORDER BY id;
REINDEX INDEX prod_idx;
SELECT id FROM prod WHERE body @@@ 'running shoes' ORDER BY id;

----------------------------------------------------------------------------
-- Writes: visibility of updated, deleted and rolled-back rows
----------------------------------------------------------------------------
-- The old version stays in the store, and the heap fetch hides it.
UPDATE prod SET body = 'Running sandals' WHERE id = 1;
SELECT id, body FROM prod WHERE body @@@ 'running shoes' ORDER BY id;
SELECT id, body FROM prod WHERE body @@@ 'sandals' ORDER BY id;
SELECT id, body FROM prod WHERE body @@@ 'running' ORDER BY id;
SELECT count(*) FROM pg_temp.store(:idx);

-- Rows reach the store at COMMIT, so the index does not find a transaction's
-- own inserts before then; a rolled-back insert never reaches it.
BEGIN;
INSERT INTO prod VALUES (6, 'rolled back shoes', '{gone}', 1);
SELECT id FROM prod WHERE body @@@ 'rolled' ORDER BY id;
ROLLBACK;
SELECT id FROM prod WHERE body @@@ 'rolled' ORDER BY id;
SELECT count(*) FROM pg_temp.store(:idx) WHERE body LIKE 'rolled%';

DELETE FROM prod WHERE id = 2;
SELECT id FROM prod WHERE body @@@ 'boots' ORDER BY id;
SELECT ctid, body FROM pg_temp.store(:idx) WHERE body IN ('Walking boots', 'Running shoes for runners') ORDER BY ctid;

-- VACUUM removes the dead versions from the store, and merges its parts once
-- enough of it is dead.
SET client_min_messages = debug1;
VACUUM prod;
SET client_min_messages = warning;
SELECT * FROM pg_temp.store(:idx) ORDER BY ctid;
SELECT id FROM prod WHERE body @@? 'running boots sandals' ORDER BY id;

-- REINDEX rebuilds the store from the live rows.
REINDEX INDEX prod_idx;
SELECT * FROM pg_temp.store(:idx) ORDER BY ctid;
SELECT id FROM prod WHERE body @@? 'running line' ORDER BY id;

-- The chdb extension's own functions still work in the same backend.
SELECT * FROM chdb_query('SELECT 42') AS (answer int);

----------------------------------------------------------------------------
-- DROP INDEX drops the store at commit
----------------------------------------------------------------------------
DROP INDEX prod_idx;
SELECT * FROM chdb_search_query(format(
    'SELECT count() FROM system.databases WHERE name = ''idx_%s''', :idx
)) AS (n bigint);
SELECT id FROM prod WHERE body @@@ 'running' ORDER BY id;

DROP TABLE prod;
DROP EXTENSION chdb_search;

-- The worker stays connected to the database, which could not be dropped for
-- the next run, so stop it.
DO $$
BEGIN
    PERFORM pg_terminate_backend(pid, 10000)
       FROM pg_stat_activity
      WHERE backend_type = 'chdb_search worker' AND datname = current_database();
END $$;
