-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION chdb_search" to load this file. \quit

-- The first of the script's three parts, which the Makefile joins into
-- sql/chdb_search--<version>.sql: this file, then sql/chdb_search_query.sql
-- (the chdb.query type), then sql/chdb_search_opclass.sql (the operators
-- and operator classes, which name that type).

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

-- The ClickHouse table behind a chdb index, idx_<oid>.t_<generation>, for
-- reading its rows through chdb_search_query.
CREATE FUNCTION chdb_search_store_table(regclass) RETURNS TEXT
AS 'MODULE_PATHNAME', 'chdb_search_debug_store_table'
LANGUAGE C STRICT;

-- The metapage of a chdb index, which ties it to its store: the magic (the
-- bytes "CHDS" as a number), the version, the generation that names the
-- store table, and the WAL position of the last flush.
CREATE FUNCTION chdb_search_metapage(
    regclass,
    OUT magic bigint, OUT version integer, OUT generation text, OUT flushed_lsn pg_lsn
) RETURNS record
AS 'MODULE_PATHNAME', 'chdb_search_debug_metapage'
LANGUAGE C STRICT;

-- The blobs of a chdb index's storage, as the worker keeps them for the
-- engine: the key libchdb chose, the size, and when the write committed.
CREATE FUNCTION chdb_search_blobs(
    regclass, OUT key text, OUT size bigint, OUT mtime timestamptz
) RETURNS SETOF record
AS 'MODULE_PATHNAME', 'chdb_search_debug_blobs'
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
REVOKE EXECUTE ON FUNCTION chdb_search_store_table(regclass) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_metapage(regclass) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_blobs(regclass) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_engine_pid() FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_debug_kill_engine(integer) FROM PUBLIC;

-- Index access method and support functions. The extension's schema is
-- chdb (the control file sets it), so everything below lives there: add it
-- to search_path, or write OPERATOR(chdb.@@@) and chdb.text_ops. Operators
-- and operator classes are looked up only in schemas one may use, so every
-- role gets USAGE. CREATE EXTENSION made the schema unless it existed
-- already (the chdb extension's, say); then this grant widens a schema the
-- extension does not own, and DROP EXTENSION, which leaves such a schema
-- alone, leaves the grant too.
GRANT USAGE ON SCHEMA chdb TO PUBLIC;
CREATE FUNCTION chdb_search_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE ACCESS METHOD chdb TYPE INDEX HANDLER chdb_search_handler;
COMMENT ON ACCESS METHOD chdb IS 'ClickHouse-backed full-text and columnar index';

-- Operator class options: per-column tokenizer, preprocessor, ngram_size,
-- support_phrase_search and (superusers only) raw_preprocessor.
CREATE FUNCTION chdb_search_text_options(internal)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- Operator classes without options still need the support function: it makes
-- an unknown option an error, and it gives a columnar_ops class its member.
-- Support function 1 also tells the access method how a column is stored
-- and searched: a class naming chdb_search_text_options gets a text skip
-- index, one naming chdb_search_text_array_options an array one, and any
-- other a plain column.
CREATE FUNCTION chdb_search_no_options(internal)
RETURNS void
AS 'MODULE_PATHNAME', 'chdb_search_no_options'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION chdb_search_text_array_options(internal)
RETURNS void
AS 'MODULE_PATHNAME', 'chdb_search_text_array_options'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION chdb_search_restrict_sel(internal, oid, internal, integer)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION chdb_search_join_sel(internal, oid, internal, smallint, internal)
RETURNS float8
AS 'MODULE_PATHNAME'
LANGUAGE C STABLE STRICT PARALLEL SAFE;

----------------------------------------------------------------------------
-- Search predicates, schema chdb
----------------------------------------------------------------------------
-- Each has a Postgres implementation so that sequential scans and rechecks
-- agree with the index. It implements ClickHouse's default pipeline only,
-- lowerUTF8 then splitByNonAlpha: tokens are runs of ASCII letters and digits
-- (plus any non-ASCII byte), compared in lower case. An index built with
-- another tokenizer or preprocessor can answer differently, so the operators
-- are only meaningful through such an index.
CREATE FUNCTION chdb.has_all_tokens(text, text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'chdb_search_has_all_tokens'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 10;

CREATE FUNCTION chdb.has_any_tokens(text, text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'chdb_search_has_any_tokens'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 10;

CREATE FUNCTION chdb.has_token(text, text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'chdb_search_has_token'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 10;

CREATE FUNCTION chdb.has_phrase(text, text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'chdb_search_has_phrase'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 10;

-- Array variants follow the array tokenizer: each element is one token, and
-- the needle is one token, so all, any and token mean the same.
CREATE FUNCTION chdb.has_all_tokens(text[], text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'chdb_search_array_has_token'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 10;

CREATE FUNCTION chdb.has_any_tokens(text[], text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'chdb_search_array_has_token'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 10;

CREATE FUNCTION chdb.has_token(text[], text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'chdb_search_array_has_token'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 10;

-- Pattern predicates, which match the text rather than its tokens: a
-- regular expression (RE2's in ClickHouse, Postgres's here, which agree on
-- the everyday syntax) and a LIKE pattern with % and _. Both follow the
-- column's preprocessor: with the default lowerUTF8 the text is lowercased
-- and the pattern matched without regard to case.
CREATE FUNCTION chdb.regex(text, text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'chdb_search_regex'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 10;

CREATE FUNCTION chdb.wildcard(text, text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'chdb_search_wildcard'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 10;

-- What the worker's tokens() makes of a string. Asks the ClickHouse side, so
-- it is not parallel safe and needs the worker.
CREATE FUNCTION chdb.tokens(text)
RETURNS text[]
AS 'MODULE_PATHNAME', 'chdb_search_tokens'
LANGUAGE C STRICT;

-- The relevance score of a row to the query's text searches, computed by
-- the custom scan in ClickHouse: the argument is any column of the indexed
-- table and only binds the call to it; the second form restricts the score
-- to one indexed text column. Postgres never evaluates it: outside a custom
-- scan it raises. Stable, so the planner leaves it to the scan.
CREATE FUNCTION chdb.score(anyelement)
RETURNS real
AS 'MODULE_PATHNAME', 'chdb_search_score'
LANGUAGE C STABLE PARALLEL SAFE;

CREATE FUNCTION chdb.score(anyelement, text)
RETURNS real
AS 'MODULE_PATHNAME', 'chdb_search_score'
LANGUAGE C STABLE PARALLEL SAFE;
