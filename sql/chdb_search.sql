-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION chdb_search" to load this file. \quit

CREATE FUNCTION chdb_search_version() RETURNS TEXT
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Debug functions, which talk to the worker about a scratch chDB database
-- named idx_0. EXECUTE is revoked from PUBLIC below, so only the extension's
-- owner (a superuser: the control file says superuser = true) or a role
-- granted EXECUTE can call them.
-- The generation is that of the store table the statement works on, as the
-- access method would send it; zero, the default, for none.
CREATE FUNCTION chdb_search_exec(TEXT, generation BIGINT DEFAULT 0) RETURNS VOID
AS 'MODULE_PATHNAME', 'chdb_search_debug_exec'
LANGUAGE C STRICT;

CREATE FUNCTION chdb_search_drop() RETURNS VOID
AS 'MODULE_PATHNAME', 'chdb_search_debug_drop'
LANGUAGE C STRICT;

CREATE FUNCTION chdb_search_query(TEXT) RETURNS SETOF record
AS 'MODULE_PATHNAME', 'chdb_search_debug_query'
LANGUAGE C STRICT;

-- Streams every row of a heap table into the given INSERT statement, e.g.
-- chdb_search_copy_to('docs', 'INSERT INTO idx_0.t (id, body)').
CREATE FUNCTION chdb_search_copy_to(regclass, TEXT) RETURNS BIGINT
AS 'MODULE_PATHNAME', 'chdb_search_debug_copy_to'
LANGUAGE C STRICT;

-- The process that runs libchdb for this database's worker; NULL until a
-- request has started it.
CREATE FUNCTION chdb_search_engine_pid() RETURNS INTEGER
AS 'MODULE_PATHNAME', 'chdb_search_debug_engine_pid'
LANGUAGE C STRICT;

-- Sends a signal to that process, as a crash would, and returns its pid.
CREATE FUNCTION chdb_search_debug_kill_engine(INTEGER) RETURNS INTEGER
AS 'MODULE_PATHNAME', 'chdb_search_debug_kill_engine'
LANGUAGE C STRICT;

REVOKE EXECUTE ON FUNCTION chdb_search_exec(text, bigint) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_drop() FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_query(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_copy_to(regclass, text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_engine_pid() FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_debug_kill_engine(integer) FROM PUBLIC;
