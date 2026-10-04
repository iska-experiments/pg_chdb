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
    flag bool
);
INSERT INTO docs VALUES
    (1, 'Running shoes for runners', 'Shoes', '{sport,shoes}', 'ann', 49.90, '2026-01-02 03:04:05+00', true),
    (2, 'Walking boots', 'Boots', '{outdoor}', 'bob', 89.00, '2026-02-03 00:00:00+00', false);
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
