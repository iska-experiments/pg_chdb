-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION chdb_search" to load this file. \quit

CREATE FUNCTION chdb_search_version() RETURNS TEXT
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Debug functions, which talk to the worker about a scratch chDB database
-- named idx_0. Superuser only.
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

-- The ClickHouse table behind a chdb index, idx_<oid>.t_<generation>, for
-- reading its rows through chdb_search_query.
CREATE FUNCTION chdb_search_store_table(regclass) RETURNS TEXT
AS 'MODULE_PATHNAME', 'chdb_search_debug_store_table'
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

REVOKE EXECUTE ON FUNCTION chdb_search_exec(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_drop() FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_query(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_copy_to(regclass, text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_store_table(regclass) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_engine_pid() FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION chdb_search_debug_kill_engine(integer) FROM PUBLIC;

-- Index access method and support functions. The handler and the options
-- functions live in the extension's schema, the search predicates in `chdb`.
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
CREATE FUNCTION chdb_search_no_options(internal)
RETURNS void
AS 'MODULE_PATHNAME', 'chdb_search_no_options'
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
CREATE SCHEMA chdb;

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

-- What the worker's tokens() makes of a string. Asks the ClickHouse side, so
-- it is not parallel safe and needs the worker.
CREATE FUNCTION chdb.tokens(text)
RETURNS text[]
AS 'MODULE_PATHNAME', 'chdb_search_tokens'
LANGUAGE C STRICT;

----------------------------------------------------------------------------
-- Operators. The estimators say "selective" and leave the rest to the index.
----------------------------------------------------------------------------
CREATE OPERATOR @@@ (
    LEFTARG = text, RIGHTARG = text, FUNCTION = chdb.has_all_tokens,
    RESTRICT = chdb_search_restrict_sel, JOIN = chdb_search_join_sel
);
CREATE OPERATOR @@? (
    LEFTARG = text, RIGHTARG = text, FUNCTION = chdb.has_any_tokens,
    RESTRICT = chdb_search_restrict_sel, JOIN = chdb_search_join_sel
);
CREATE OPERATOR @@= (
    LEFTARG = text, RIGHTARG = text, FUNCTION = chdb.has_token,
    RESTRICT = chdb_search_restrict_sel, JOIN = chdb_search_join_sel
);
CREATE OPERATOR @@~ (
    LEFTARG = text, RIGHTARG = text, FUNCTION = chdb.has_phrase,
    RESTRICT = chdb_search_restrict_sel, JOIN = chdb_search_join_sel
);

CREATE OPERATOR @@@ (
    LEFTARG = text[], RIGHTARG = text, FUNCTION = chdb.has_all_tokens,
    RESTRICT = chdb_search_restrict_sel, JOIN = chdb_search_join_sel
);
CREATE OPERATOR @@? (
    LEFTARG = text[], RIGHTARG = text, FUNCTION = chdb.has_any_tokens,
    RESTRICT = chdb_search_restrict_sel, JOIN = chdb_search_join_sel
);
CREATE OPERATOR @@= (
    LEFTARG = text[], RIGHTARG = text, FUNCTION = chdb.has_token,
    RESTRICT = chdb_search_restrict_sel, JOIN = chdb_search_join_sel
);

----------------------------------------------------------------------------
-- Operator classes. Strategies: 1 has_all_tokens @@@, 2 has_any_tokens @@?,
-- 3 has_token @@=, 4 has_phrase @@~ (needs support_phrase_search).
----------------------------------------------------------------------------
CREATE OPERATOR CLASS text_ops
DEFAULT FOR TYPE text USING chdb AS
    OPERATOR 1 @@@ (text, text),
    OPERATOR 2 @@? (text, text),
    OPERATOR 3 @@= (text, text),
    OPERATOR 4 @@~ (text, text),
    FUNCTION 1 (text) chdb_search_text_options(internal);

CREATE OPERATOR CLASS text_array_ops
FOR TYPE text[] USING chdb AS
    OPERATOR 1 @@@ (text[], text),
    OPERATOR 2 @@? (text[], text),
    OPERATOR 3 @@= (text[], text),
    FUNCTION 1 (text[]) chdb_search_no_options(internal);

-- One class for every stored column type, so that columns are written
-- `author columnar_ops` whatever they hold. It is declared for anyelement; the
-- comparison operators of each supported type are members of its family.
-- Strategies: 1 =, 2 <, 3 <=, 4 >, 5 >=. Being the only default for types
-- without one of their own, it applies when a column names no class.
CREATE OPERATOR CLASS columnar_ops
DEFAULT FOR TYPE anyelement USING chdb AS
    FUNCTION 1 (anyelement) chdb_search_no_options(internal);

ALTER OPERATOR FAMILY columnar_ops USING chdb ADD
    OPERATOR 1 = (int2, int2),
    OPERATOR 2 < (int2, int2),
    OPERATOR 3 <= (int2, int2),
    OPERATOR 4 > (int2, int2),
    OPERATOR 5 >= (int2, int2),
    OPERATOR 1 = (int4, int4),
    OPERATOR 2 < (int4, int4),
    OPERATOR 3 <= (int4, int4),
    OPERATOR 4 > (int4, int4),
    OPERATOR 5 >= (int4, int4),
    OPERATOR 1 = (int8, int8),
    OPERATOR 2 < (int8, int8),
    OPERATOR 3 <= (int8, int8),
    OPERATOR 4 > (int8, int8),
    OPERATOR 5 >= (int8, int8),
    OPERATOR 1 = (float4, float4),
    OPERATOR 2 < (float4, float4),
    OPERATOR 3 <= (float4, float4),
    OPERATOR 4 > (float4, float4),
    OPERATOR 5 >= (float4, float4),
    OPERATOR 1 = (float8, float8),
    OPERATOR 2 < (float8, float8),
    OPERATOR 3 <= (float8, float8),
    OPERATOR 4 > (float8, float8),
    OPERATOR 5 >= (float8, float8),
    OPERATOR 1 = (numeric, numeric),
    OPERATOR 2 < (numeric, numeric),
    OPERATOR 3 <= (numeric, numeric),
    OPERATOR 4 > (numeric, numeric),
    OPERATOR 5 >= (numeric, numeric),
    OPERATOR 1 = (bool, bool),
    OPERATOR 2 < (bool, bool),
    OPERATOR 3 <= (bool, bool),
    OPERATOR 4 > (bool, bool),
    OPERATOR 5 >= (bool, bool),
    OPERATOR 1 = (date, date),
    OPERATOR 2 < (date, date),
    OPERATOR 3 <= (date, date),
    OPERATOR 4 > (date, date),
    OPERATOR 5 >= (date, date),
    OPERATOR 1 = (timestamp, timestamp),
    OPERATOR 2 < (timestamp, timestamp),
    OPERATOR 3 <= (timestamp, timestamp),
    OPERATOR 4 > (timestamp, timestamp),
    OPERATOR 5 >= (timestamp, timestamp),
    OPERATOR 1 = (timestamptz, timestamptz),
    OPERATOR 2 < (timestamptz, timestamptz),
    OPERATOR 3 <= (timestamptz, timestamptz),
    OPERATOR 4 > (timestamptz, timestamptz),
    OPERATOR 5 >= (timestamptz, timestamptz),
    OPERATOR 1 = (uuid, uuid),
    OPERATOR 2 < (uuid, uuid),
    OPERATOR 3 <= (uuid, uuid),
    OPERATOR 4 > (uuid, uuid),
    OPERATOR 5 >= (uuid, uuid),
    OPERATOR 1 = (text, text),
    OPERATOR 2 < (text, text),
    OPERATOR 3 <= (text, text),
    OPERATOR 4 > (text, text),
    OPERATOR 5 >= (text, text);
