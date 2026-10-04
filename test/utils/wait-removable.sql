\set ECHO errors
-- VACUUM removes a dead row only once no other session's snapshot can see
-- it, and autovacuum's ANALYZE holds one now and then. A snapshot taken
-- while no older transaction runs cannot, so a test whose VACUUM must remove
-- a row CALLs pg_temp.wait_removable() first: it waits (ten seconds at most)
-- until no other session of the database holds a snapshot or a transaction
-- ID.
CREATE PROCEDURE pg_temp.wait_removable() LANGUAGE plpgsql AS $$
DECLARE
    deadline timestamptz := clock_timestamp() + interval '10 seconds';
BEGIN
    WHILE clock_timestamp() < deadline AND EXISTS (
        SELECT FROM pg_stat_activity
         WHERE datname = current_database() AND pid <> pg_backend_pid()
           AND (backend_xmin IS NOT NULL OR backend_xid IS NOT NULL)
    ) LOOP
        PERFORM pg_sleep(0.01);
        PERFORM pg_stat_clear_snapshot();
    END LOOP;
END
$$;
\set ECHO all
