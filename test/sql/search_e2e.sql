-- The chdb index access method end to end: the searches below are answered by
-- the database's chdb_search worker from the index's ClickHouse table. The
-- server needs libchdb on its library path or chdb_search.libchdb_path.
\set VERBOSITY terse
SET client_min_messages = warning;
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;
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

-- The index's ClickHouse table, idx_<oid>.t_<generation>, read through the
-- worker. ctid is the heap TID packed as (block << 16) | offset; a NULL array
-- is stored empty.
SELECT 'prod_idx'::regclass::oid AS idx \gset
SELECT chdb_search_store_table('prod_idx') AS tbl \gset
SELECT :'tbl' ~ ('^idx_' || :idx || '\.t_\d+$') AS named_by_generation;
CREATE FUNCTION pg_temp.store(text)
RETURNS TABLE (ctid bigint, body text, tags text[], price numeric)
LANGUAGE sql AS $$
    SELECT * FROM chdb_search_query(format(
        'SELECT toInt64(ctid), body, tags, price FROM %s ORDER BY ctid', $1
    )) AS (ctid bigint, body text, tags text[], price numeric)
$$;
-- The generation tables of the store; the meta table beside them stays.
CREATE FUNCTION pg_temp.tables(oid)
RETURNS bigint
LANGUAGE sql AS $$
    SELECT n FROM chdb_search_query(format(
        'SELECT count() FROM system.tables WHERE database = ''idx_%s'' AND name LIKE ''t\_%%''', $1
    )) AS (n bigint)
$$;
SELECT * FROM pg_temp.store(:'tbl') ORDER BY ctid;

-- Every heap row's TID packs to the store row holding its values.
SELECT p.id, p.ctid, s.ctid AS packed,
       (s.body, s.price) IS NOT DISTINCT FROM (p.body, p.price) AS same
  FROM prod p
  LEFT JOIN pg_temp.store(:'tbl') s
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
SELECT chdb_search_exec(format('DELETE FROM %s WHERE ctid = 3', :'tbl'));
SELECT id FROM prod WHERE body @@@ 'running shoes' ORDER BY id;
SET enable_seqscan = on;
SET enable_indexscan = off;
SELECT id FROM prod WHERE body @@@ 'running shoes' ORDER BY id;
RESET enable_indexscan;
SET enable_seqscan = off;
-- A search ClickHouse cannot run raises, rather than finding nothing.
SELECT chdb_search_exec(format('DROP TABLE %s', :'tbl'));
SELECT id FROM prod WHERE body @@@ 'running shoes' ORDER BY id;
REINDEX INDEX prod_idx;
SELECT chdb_search_store_table('prod_idx') AS tbl \gset
SELECT id FROM prod WHERE body @@@ 'running shoes' ORDER BY id;

----------------------------------------------------------------------------
-- Writes: visibility of updated, deleted and rolled-back rows
----------------------------------------------------------------------------
-- The old version stays in the store, and the heap fetch hides it.
UPDATE prod SET body = 'Running sandals' WHERE id = 1;
SELECT id, body FROM prod WHERE body @@@ 'running shoes' ORDER BY id;
SELECT id, body FROM prod WHERE body @@@ 'sandals' ORDER BY id;
SELECT id, body FROM prod WHERE body @@@ 'running' ORDER BY id;
SELECT count(*) FROM pg_temp.store(:'tbl');

-- Rows reach the store at COMMIT, so the index does not find a transaction's
-- own inserts before then; a rolled-back insert never reaches it.
BEGIN;
INSERT INTO prod VALUES (6, 'rolled back shoes', '{gone}', 1);
SELECT id FROM prod WHERE body @@@ 'rolled' ORDER BY id;
ROLLBACK;
SELECT id FROM prod WHERE body @@@ 'rolled' ORDER BY id;
SELECT count(*) FROM pg_temp.store(:'tbl') WHERE body LIKE 'rolled%';

DELETE FROM prod WHERE id = 2;
SELECT id FROM prod WHERE body @@@ 'boots' ORDER BY id;
SELECT ctid, body FROM pg_temp.store(:'tbl') WHERE body IN ('Walking boots', 'Running shoes for runners') ORDER BY ctid;

-- VACUUM removes the dead versions from the store, and merges its parts once
-- enough of it is dead.
SET client_min_messages = debug1;
VACUUM prod;
SET client_min_messages = warning;
SELECT * FROM pg_temp.store(:'tbl') ORDER BY ctid;
SELECT id FROM prod WHERE body @@? 'running boots sandals' ORDER BY id;

-- REINDEX rebuilds the store from the live rows into a new generation. The
-- old table stays until VACUUM sweeps it: the transaction could have rolled
-- back to it.
REINDEX INDEX prod_idx;
SELECT chdb_search_store_table('prod_idx') AS tbl2 \gset
SELECT :'tbl2' <> :'tbl' AS new_generation, pg_temp.tables(:idx);
SELECT * FROM pg_temp.store(:'tbl2') ORDER BY ctid;
SELECT id FROM prod WHERE body @@? 'running line' ORDER BY id;
SET client_min_messages = debug1;
VACUUM prod;
SET client_min_messages = warning;
SELECT pg_temp.tables(:idx);
SELECT count(*) FROM pg_temp.store(:'tbl2');

-- So is a staging table whose transaction is over, as a crashed backend
-- leaves it (xid 3 is the first ever assigned).
SELECT chdb_search_exec(format('CREATE TABLE %s_tx_3 AS %s', :'tbl2', :'tbl2'));
SELECT pg_temp.tables(:idx);
SET client_min_messages = debug1;
VACUUM prod;
SET client_min_messages = warning;
SELECT pg_temp.tables(:idx);

-- A rebuild in the same transaction indexes the rows inserted before it once,
-- unless it is rolled back to a savepoint, when the buffered rows are sent
-- to the table the index kept.
BEGIN;
INSERT INTO prod VALUES (7, 'Rebuilt running shoes', '{sport}', 70.00);
REINDEX INDEX prod_idx;
COMMIT;
BEGIN;
INSERT INTO prod VALUES (8, 'Shoes kept through a rollback', '{sport}', 80.00);
SAVEPOINT s;
REINDEX INDEX prod_idx;
ROLLBACK TO s;
COMMIT;
SELECT chdb_search_store_table('prod_idx') AS tbl \gset
SELECT id FROM prod WHERE body @@@ 'shoes' ORDER BY id;
SELECT body FROM pg_temp.store(:'tbl') WHERE body ~ 'Rebuilt|kept' ORDER BY body;

-- The chdb extension's own functions still work in the same backend.
SELECT * FROM chdb_query('SELECT 42') AS (answer int);

----------------------------------------------------------------------------
-- Columnar text filters: ClickHouse compares bytewise, as COLLATE "C" does
----------------------------------------------------------------------------
CREATE TABLE coll (id int, author text COLLATE "C");
INSERT INTO coll VALUES (1, 'ann'), (2, 'Bob');
CREATE INDEX coll_idx ON coll USING chdb (author columnar_ops);
EXPLAIN (COSTS OFF) SELECT id FROM coll WHERE author >= 'a' ORDER BY id;
SELECT id FROM coll WHERE author >= 'a' ORDER BY id;
SELECT id FROM coll WHERE author < 'b' ORDER BY id;
SET enable_seqscan = on;
SET enable_indexscan = off;
SELECT id FROM coll WHERE author >= 'a' ORDER BY id;
SELECT id FROM coll WHERE author < 'b' ORDER BY id;
RESET enable_indexscan;
SET enable_seqscan = off;
DROP TABLE coll;

----------------------------------------------------------------------------
-- Column names are quoted: a keyword or a float literal's spelling works
----------------------------------------------------------------------------
CREATE TABLE kw ("index" text, inf float8, nan float8);
INSERT INTO kw VALUES ('a row', 1, 2);
CREATE INDEX kw_idx ON kw USING chdb ("index", inf, nan);
SELECT * FROM kw WHERE inf = 1;
SELECT * FROM kw WHERE nan > 0 AND "index" @@@ 'row';
DROP TABLE kw;

----------------------------------------------------------------------------
-- ORDER BY a distance operator: rows come back nearest first, and a NULL
-- argument leaves every row with a NULL distance
----------------------------------------------------------------------------
ALTER OPERATOR FAMILY columnar_ops USING chdb ADD OPERATOR 1 <-> (point, point) FOR ORDER BY float_ops;
CREATE TABLE pts (id int, loc point);
INSERT INTO pts VALUES (1, '(0,0)'), (2, '(10,10)'), (3, '(1,1)'), (4, NULL);
CREATE INDEX pts_idx ON pts USING chdb (loc);
EXPLAIN (COSTS OFF) SELECT id FROM pts ORDER BY loc <-> '(0,0)' LIMIT 3;
SELECT id, loc <-> '(0,0)' AS distance FROM pts ORDER BY loc <-> '(0,0)' LIMIT 3;
SELECT id FROM pts ORDER BY loc <-> '(10,10)';
SET plan_cache_mode = force_generic_plan;
PREPARE near(point) AS SELECT count(*) FROM (SELECT id FROM pts ORDER BY loc <-> $1 LIMIT 10) q;
EXECUTE near(NULL);
EXECUTE near('(1,1)');
RESET plan_cache_mode;
DROP TABLE pts;
ALTER OPERATOR FAMILY columnar_ops USING chdb DROP OPERATOR 1 (point, point);

----------------------------------------------------------------------------
-- Floats compare at their own width, NaN and the infinities as in Postgres
----------------------------------------------------------------------------
CREATE TABLE nums (id int, f4 float4, f8 float8, d date);
INSERT INTO nums VALUES (1, 0.1, 'NaN', '2026-01-01'), (2, 1.5, 1.5, '2000-01-01'), (3, NULL, NULL, NULL);
CREATE INDEX nums_idx ON nums USING chdb (f4, f8, d);
CREATE FUNCTION pg_temp.num_checks() RETURNS TABLE (q text, ids int[]) LANGUAGE sql AS $$
    SELECT 'f4 = 0.1', array_agg(id ORDER BY id) FROM nums WHERE f4 = 0.1::real
    UNION ALL SELECT 'f4 > 0.1', array_agg(id ORDER BY id) FROM nums WHERE f4 > 0.1::real
    UNION ALL SELECT 'f4 = 0.1::float8', array_agg(id ORDER BY id) FROM nums WHERE f4 = 0.1
    UNION ALL SELECT 'f4 > 0.1::float8', array_agg(id ORDER BY id) FROM nums WHERE f4 > 0.1
    UNION ALL SELECT 'f8 = NaN', array_agg(id ORDER BY id) FROM nums WHERE f8 = 'NaN'
    UNION ALL SELECT 'f8 < NaN', array_agg(id ORDER BY id) FROM nums WHERE f8 < 'NaN'
    UNION ALL SELECT 'f8 <= NaN', array_agg(id ORDER BY id) FROM nums WHERE f8 <= 'NaN'
    UNION ALL SELECT 'f8 > NaN', array_agg(id ORDER BY id) FROM nums WHERE f8 > 'NaN'
    UNION ALL SELECT 'd < infinity', array_agg(id ORDER BY id) FROM nums WHERE d < 'infinity'
    UNION ALL SELECT 'd > -infinity', array_agg(id ORDER BY id) FROM nums WHERE d > '-infinity'
    UNION ALL SELECT 'd = infinity', array_agg(id ORDER BY id) FROM nums WHERE d = 'infinity'
$$;
EXPLAIN (COSTS OFF) SELECT id FROM nums WHERE f8 <= 'NaN';
SELECT * FROM pg_temp.num_checks();
SET enable_seqscan = on;
SET enable_indexscan = off;
SELECT * FROM pg_temp.num_checks();
RESET enable_indexscan;
SET enable_seqscan = off;
DROP TABLE nums;

----------------------------------------------------------------------------
-- text[]: elements compare in lower case through the index as by seqscan,
-- and the needle is one token however many words it has
----------------------------------------------------------------------------
INSERT INTO prod VALUES (9, 'Paint', '{Red,Blue}', 1.00);
SELECT id FROM prod WHERE tags @@@ 'BLUE';
SELECT id FROM prod WHERE tags @@? 'blue';
SELECT id FROM prod WHERE tags @@= 'BLUE';
SELECT id FROM prod WHERE tags @@@ 'Red Blue';
SET enable_seqscan = on;
SET enable_indexscan = off;
SELECT id FROM prod WHERE tags @@@ 'BLUE';
SELECT id FROM prod WHERE tags @@? 'blue';
SELECT id FROM prod WHERE tags @@= 'BLUE';
SELECT id FROM prod WHERE tags @@@ 'Red Blue';
RESET enable_indexscan;
SET enable_seqscan = off;
-- Non-ASCII letters lowercase alike in the index and in the predicates.
INSERT INTO prod VALUES (10, 'ÉCOLE in İstanbul', '{ÖL}', 2.00);
SELECT id FROM prod WHERE body @@@ 'école İSTANBUL' AND tags @@= 'öl';
SET enable_seqscan = on;
SET enable_indexscan = off;
SELECT id FROM prod WHERE body @@@ 'école İSTANBUL' AND tags @@= 'öl';
RESET enable_indexscan;
SET enable_seqscan = off;

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
