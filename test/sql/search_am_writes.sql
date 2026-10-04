-- Writes through the chdb index access method. Rows are buffered per
-- transaction and sent to the store at COMMIT, dropped at ROLLBACK and
-- rewound by ROLLBACK TO a savepoint; every INSERT the buffer sends is logged
-- at DEBUG1 with the rows it carries. The output is the same with the worker
-- and with the stub client (see search_am.sql); what reaches the store is
-- checked by search_e2e.
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
CREATE INDEX docs_title ON docs USING chdb (title);

----------------------------------------------------------------------------
-- Buffered, flushed at COMMIT, dropped at ROLLBACK
----------------------------------------------------------------------------
SET client_min_messages = debug1;
BEGIN;
INSERT INTO docs VALUES (3, 'committed row', 'T', '{a}', 'cy', 1, now(), true);
\echo -- nothing is sent before COMMIT
COMMIT;
\echo -- rolled back
BEGIN;
INSERT INTO docs VALUES (4, 'rolled back row', 'T', '{a}', 'dee', 1, now(), true);
ROLLBACK;
\echo -- savepoints
BEGIN;
INSERT INTO docs VALUES (5, 'kept', 'T', '{a}', 'eve', 1, now(), true);
SAVEPOINT s1;
INSERT INTO docs VALUES (6, 'dropped by rollback to', 'T', '{a}', 'fay', 1, now(), true);
ROLLBACK TO s1;
SAVEPOINT s2;
INSERT INTO docs VALUES (7, 'released', 'T', '{a}', 'gus', 1, now(), true);
RELEASE s2;
COMMIT;
\echo -- update and delete
UPDATE docs SET body = 'changed' WHERE id = 1;
DELETE FROM docs WHERE id = 2;
\echo -- rows with NULLs
INSERT INTO docs (id) VALUES (8);
\echo -- a failing statement
INSERT INTO docs VALUES (1, 'duplicate key');
RESET client_min_messages;

----------------------------------------------------------------------------
-- Nested savepoints: each level rewinds to its own checkpoint
----------------------------------------------------------------------------
SET client_min_messages = debug1;
\echo -- two levels rolled back in turn
BEGIN;
INSERT INTO docs (id, body) VALUES (20, 'x');
SAVEPOINT a;
INSERT INTO docs (id, body) VALUES (21, 'y');
SAVEPOINT b;
INSERT INTO docs (id, body) VALUES (22, 'z');
ROLLBACK TO b;
ROLLBACK TO a;
COMMIT;
\echo -- a level released into one that is then rolled back
BEGIN;
SAVEPOINT a;
SAVEPOINT b;
INSERT INTO docs (id, body) VALUES (15, 'f');
RELEASE b;
SAVEPOINT c;
INSERT INTO docs (id, body) VALUES (16, 'g');
ROLLBACK TO c;
ROLLBACK TO a;
COMMIT;
\echo -- released and rolled back levels mixed
BEGIN;
INSERT INTO docs (id, body) VALUES (10, 'a'), (11, 'b');
SAVEPOINT a;
INSERT INTO docs (id, body) VALUES (12, 'c');
SAVEPOINT b;
INSERT INTO docs (id, body) VALUES (13, 'd');
RELEASE b;
ROLLBACK TO a;
SAVEPOINT c;
INSERT INTO docs (id, body) VALUES (14, 'e');
RELEASE c;
COMMIT;
\echo -- nested PL/pgSQL exception blocks, the inner one rolled back first
DO $$
BEGIN
    BEGIN
        INSERT INTO docs (id, body) VALUES (30, 'p');
        BEGIN
            INSERT INTO docs (id, body) VALUES (31, 'q');
            RAISE 'y';
        EXCEPTION WHEN OTHERS THEN
            NULL;
        END;
        RAISE 'x';
    EXCEPTION WHEN OTHERS THEN
        NULL;
    END;
END $$;
\echo -- a handler that inserts after the rollback of the block it handles
DO $$
BEGIN
    BEGIN
        INSERT INTO docs (id, body) VALUES (32, 'p');
        RAISE 'x';
    EXCEPTION WHEN OTHERS THEN
        BEGIN
            INSERT INTO docs (id, body) VALUES (33, 'q');
            RAISE 'y';
        EXCEPTION WHEN OTHERS THEN
            INSERT INTO docs (id, body) VALUES (34, 'r');
        END;
    END;
END $$;
RESET client_min_messages;

----------------------------------------------------------------------------
-- Inside a savepoint the buffer is not staged early: it warns, then it caps
----------------------------------------------------------------------------
SET chdb_search.flush_threshold = '64kB';
SET chdb_search.max_buffer = '128kB';
SET client_min_messages = debug1;
BEGIN;
SAVEPOINT s;
INSERT INTO docs (id, body) SELECT 1000 + i, repeat('word ', 250) FROM generate_series(1, 60) i;
INSERT INTO docs (id, body) SELECT 2000 + i, repeat('word ', 250) FROM generate_series(1, 60) i;
ROLLBACK TO s;
COMMIT;
\echo -- a PL/pgSQL block that completes keeps its rows for COMMIT
DO $$
BEGIN
    INSERT INTO docs (id, body) SELECT 3000 + i, repeat('word ', 250) FROM generate_series(1, 60) i;
EXCEPTION WHEN OTHERS THEN
    NULL;
END $$;
\echo -- at the top level the same rows are staged instead
BEGIN;
INSERT INTO docs (id, body) SELECT 4000 + i, repeat('word ', 250) FROM generate_series(1, 60) i;
ROLLBACK;
RESET client_min_messages;
RESET chdb_search.max_buffer;
RESET chdb_search.flush_threshold;

----------------------------------------------------------------------------
-- Staging: past the threshold a transaction's rows go to <table>_tx_<xid>
----------------------------------------------------------------------------
SET chdb_search.flush_threshold = '64kB';
SET client_min_messages = debug1;
BEGIN;
INSERT INTO docs (id, body) SELECT 10000 + i, repeat('word ', 250) FROM generate_series(1, 120) i;
INSERT INTO docs (id, body) VALUES (11000, 'tail');
COMMIT;
\echo -- a rollback drops the staging table, once
BEGIN;
INSERT INTO docs (id, body) SELECT 12000 + i, repeat('word ', 250) FROM generate_series(1, 120) i;
ROLLBACK;
\echo -- staged rows survive a savepoint rolled back after them
BEGIN;
INSERT INTO docs (id, body) SELECT 13000 + i, repeat('word ', 250) FROM generate_series(1, 120) i;
SAVEPOINT s;
INSERT INTO docs (id, body) VALUES (14000, 'sp');
ROLLBACK TO s;
COMMIT;
\echo -- two indexes over the threshold: one staging table each
BEGIN;
INSERT INTO docs (id, body, title)
SELECT 15000 + i, repeat('word ', 250), repeat('title ', 200) FROM generate_series(1, 120) i;
ROLLBACK;
RESET client_min_messages;
RESET chdb_search.flush_threshold;

----------------------------------------------------------------------------
-- VACUUM: what it deletes depends on the store's rows (see search_e2e); it
-- also looks for tables of other generations and of finished transactions
----------------------------------------------------------------------------
VACUUM docs;
-- (Nothing is dead now, so only the sweep's statements show.)
SET client_min_messages = debug1;
VACUUM docs;
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
