-- The SELECT statements the chdb index access method generates for scans,
-- logged at DEBUG1: how search keys, filters and order-by keys are rendered
-- and how literals are quoted. The output is the same with the worker and
-- with the stub client (see search_am.sql): the rows a scan returns are
-- discarded here and checked by search_e2e.
\set VERBOSITY terse
CREATE EXTENSION chdb_search;
SET search_path = public, chdb;
SET chdb_search.mask_oids = on;

CREATE TABLE docs (
    id int PRIMARY KEY,
    body text,
    title text,
    tags text[],
    author text COLLATE "C",
    price numeric(10, 2),
    seen timestamptz,
    flag bool,
    loc point
);
INSERT INTO docs VALUES
    (1, 'Running shoes for runners', 'Shoes', '{sport,shoes}', 'ann', 49.90, '2026-01-02 03:04:05+00', true, '(1,2)'),
    (2, 'Walking boots', 'Boots', '{outdoor}', 'bob', 89.00, '2026-02-03 00:00:00+00', false, '(3,4)');
CREATE INDEX docs_idx ON docs USING chdb (
    body, title, tags text_array_ops, author columnar_ops, price, seen, flag columnar_ops
);
SET enable_seqscan = off;

----------------------------------------------------------------------------
-- Search keys and filters, with escaping of the search string
----------------------------------------------------------------------------
SET client_min_messages = debug1;
\o /dev/null
SELECT id FROM docs WHERE body @@@ 'running shoes';
SELECT id FROM docs WHERE body @@? 'it''s a \back\slash';
SELECT id FROM docs WHERE title @@= 'Shoes' AND tags @@@ 'sport' AND author >= 'a' AND price < 50.50;
SELECT id FROM docs WHERE seen > '2026-01-01 00:00:00+00' AND flag = false;
SELECT id FROM docs WHERE body @@@ NULL;
\o
RESET client_min_messages;

----------------------------------------------------------------------------
-- Literals: parsed to the column's type, and the constants ClickHouse
-- compares unlike Postgres (NaN, the infinities) become predicates
----------------------------------------------------------------------------
CREATE TABLE types (
    i2 int2, i4 int4, i8 int8, f4 float4, f8 float8, n numeric, d date,
    ts timestamp, u uuid, v varchar(20) COLLATE "C", arr text[]
);
CREATE INDEX types_idx ON types USING chdb (i2, i4, i8, f4, f8, n, d, ts, u, v columnar_ops, arr text_array_ops);
SET client_min_messages = debug1;
\o /dev/null
SELECT * FROM types WHERE i2 = 1::int2 AND i4 = 2 AND i8 = 3::int8 AND v = 'x' AND d = '2026-01-02';
SELECT * FROM types WHERE u = '00000000-0000-0000-0000-000000000001' AND arr @@= 'x';
SELECT * FROM types WHERE f4 = 0.1::real;
SELECT * FROM types WHERE f8 = 0.1 AND f4 > 'Infinity'::real;
SELECT * FROM types WHERE f8 > 1e308;
SELECT * FROM types WHERE f8 = 'NaN'::float8;
SELECT * FROM types WHERE f8 < 'NaN';
SELECT * FROM types WHERE f8 > 'NaN';
SELECT * FROM types WHERE f8 <= 'NaN';
SELECT * FROM types WHERE f8 >= 'NaN' AND f4 = 'NaN'::real;
SELECT * FROM types WHERE ts < 'infinity';
SELECT * FROM types WHERE ts = 'infinity';
SELECT * FROM types WHERE ts >= '-infinity';
SELECT * FROM types WHERE d > '-infinity'::date;
SELECT * FROM types WHERE d <= '-infinity'::date;
SELECT * FROM types WHERE n < 'NaN'::numeric;
SELECT * FROM types WHERE n >= 'Infinity'::numeric;
SELECT * FROM types WHERE n > '-Infinity'::numeric AND n = 1.50;
PREPARE p(timestamp) AS SELECT * FROM types WHERE ts < $1;
EXECUTE p('infinity');
EXECUTE p('2026-01-01');
\o
RESET client_min_messages;
-- The store has no infinities, so the writer refuses them rather than let
-- the encoder wrap them into finite values.
INSERT INTO types (d) VALUES ('infinity');
INSERT INTO types (ts) VALUES ('-infinity');
INSERT INTO types (n) VALUES ('NaN');
DROP TABLE types;

----------------------------------------------------------------------------
-- ORDER BY a distance operator of the column's family
----------------------------------------------------------------------------
-- Postgres's own point distance stands in for the operators chdb_vector
-- will add; the ClickHouse Point is a tuple, which L2Distance takes.
ALTER OPERATOR FAMILY columnar_ops USING chdb ADD OPERATOR 1 <-> (point, point) FOR ORDER BY float_ops;
SELECT amvalidate(oid) FROM pg_opclass WHERE opcname = 'columnar_ops';
CREATE INDEX docs_loc ON docs USING chdb (loc, body);
EXPLAIN (COSTS OFF) SELECT id FROM docs ORDER BY loc <-> '(1,2)' LIMIT 2;
SET client_min_messages = debug1;
\o /dev/null
SELECT id FROM docs ORDER BY loc <-> '(1,2)' LIMIT 2;
SELECT id FROM docs WHERE body @@@ 'shoes' ORDER BY loc <-> '(1,2)', loc <-> '(3,4)' LIMIT 2;
-- A NULL argument reaches the scan from a generic plan (a custom plan folds
-- the strict operator to NULL and sorts nothing): every row then has a NULL
-- distance, as by seqscan, rather than vanishing.
SET plan_cache_mode = force_generic_plan;
PREPARE near(point) AS SELECT id FROM docs ORDER BY loc <-> $1 LIMIT 1;
EXECUTE near(NULL);
EXECUTE near('(3,4)');
RESET plan_cache_mode;
\o
RESET client_min_messages;
SELECT id FROM docs ORDER BY loc <-> '(1,NaN)' LIMIT 1;
DROP INDEX docs_loc;
ALTER OPERATOR FAMILY columnar_ops USING chdb DROP OPERATOR 1 (point, point);

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
