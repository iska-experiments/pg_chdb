-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION chdb_search" to load this file. \quit

CREATE FUNCTION chdb_search_version() RETURNS TEXT
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Debug functions, which talk to the worker about a scratch chDB database
-- named idx_0. EXECUTE is revoked from PUBLIC below, so only the extension's
-- owner (a superuser: the control file says superuser = true) or a role
-- granted EXECUTE can call them.
CREATE FUNCTION chdb_search_exec(TEXT) RETURNS VOID
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

REVOKE EXECUTE ON FUNCTION chdb_search_exec(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_drop() FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_query(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_copy_to(regclass, text) FROM PUBLIC;
