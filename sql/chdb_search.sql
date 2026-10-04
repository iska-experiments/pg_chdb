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

-- The metapage of a chdb index, which ties it to its store: the magic (the
-- bytes "CHDS" as a number), the version, the generation that names the
-- store table, and the WAL position of the last flush, which the store's
-- meta table must match for a scan to trust it.
CREATE FUNCTION chdb_search_metapage(
    regclass,
    OUT magic bigint, OUT version integer, OUT generation text, OUT flushed_lsn pg_lsn
) RETURNS record
AS 'MODULE_PATHNAME', 'chdb_search_debug_metapage'
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
REVOKE EXECUTE ON FUNCTION chdb_search_metapage(regclass) FROM PUBLIC;
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

-- The default for text[], or columnar_ops would take it through anyelement
-- and store an array nothing can search.
CREATE OPERATOR CLASS text_array_ops
DEFAULT FOR TYPE text[] USING chdb AS
    OPERATOR 1 @@@ (text[], text),
    OPERATOR 2 @@? (text[], text),
    OPERATOR 3 @@= (text[], text),
    FUNCTION 1 (text[]) chdb_search_text_array_options(internal);

-- One class for every stored column type, so that columns are written
-- `author columnar_ops` whatever they hold. It is declared for anyelement; the
-- comparison operators of each supported type are members of its family,
-- with the cross-type pairs of the integer and float families, so that
-- WHERE i2 = 1 or f4 > 0.1 is pushed down as written. Strategies: 11 =,
-- 12 <, 13 <=, 14 >, 15 >=, disjoint from the text search strategies so that
-- an operator is rendered by its number alone, whatever class it came from.
-- Being the only default for types without one of their own, it applies
-- when a column names no class; a type with no operators here is refused.
CREATE OPERATOR CLASS columnar_ops
DEFAULT FOR TYPE anyelement USING chdb AS
    FUNCTION 1 (anyelement) chdb_search_no_options(internal);

ALTER OPERATOR FAMILY columnar_ops USING chdb ADD
    OPERATOR 11 = (int2, int2),
    OPERATOR 12 < (int2, int2),
    OPERATOR 13 <= (int2, int2),
    OPERATOR 14 > (int2, int2),
    OPERATOR 15 >= (int2, int2),
    OPERATOR 11 = (int4, int4),
    OPERATOR 12 < (int4, int4),
    OPERATOR 13 <= (int4, int4),
    OPERATOR 14 > (int4, int4),
    OPERATOR 15 >= (int4, int4),
    OPERATOR 11 = (int8, int8),
    OPERATOR 12 < (int8, int8),
    OPERATOR 13 <= (int8, int8),
    OPERATOR 14 > (int8, int8),
    OPERATOR 15 >= (int8, int8),
    OPERATOR 11 = (float4, float4),
    OPERATOR 12 < (float4, float4),
    OPERATOR 13 <= (float4, float4),
    OPERATOR 14 > (float4, float4),
    OPERATOR 15 >= (float4, float4),
    OPERATOR 11 = (float8, float8),
    OPERATOR 12 < (float8, float8),
    OPERATOR 13 <= (float8, float8),
    OPERATOR 14 > (float8, float8),
    OPERATOR 15 >= (float8, float8),
    OPERATOR 11 = (numeric, numeric),
    OPERATOR 12 < (numeric, numeric),
    OPERATOR 13 <= (numeric, numeric),
    OPERATOR 14 > (numeric, numeric),
    OPERATOR 15 >= (numeric, numeric),
    OPERATOR 11 = (bool, bool),
    OPERATOR 12 < (bool, bool),
    OPERATOR 13 <= (bool, bool),
    OPERATOR 14 > (bool, bool),
    OPERATOR 15 >= (bool, bool),
    OPERATOR 11 = (date, date),
    OPERATOR 12 < (date, date),
    OPERATOR 13 <= (date, date),
    OPERATOR 14 > (date, date),
    OPERATOR 15 >= (date, date),
    OPERATOR 11 = (timestamp, timestamp),
    OPERATOR 12 < (timestamp, timestamp),
    OPERATOR 13 <= (timestamp, timestamp),
    OPERATOR 14 > (timestamp, timestamp),
    OPERATOR 15 >= (timestamp, timestamp),
    OPERATOR 11 = (timestamptz, timestamptz),
    OPERATOR 12 < (timestamptz, timestamptz),
    OPERATOR 13 <= (timestamptz, timestamptz),
    OPERATOR 14 > (timestamptz, timestamptz),
    OPERATOR 15 >= (timestamptz, timestamptz),
    OPERATOR 11 = (uuid, uuid),
    OPERATOR 12 < (uuid, uuid),
    OPERATOR 13 <= (uuid, uuid),
    OPERATOR 14 > (uuid, uuid),
    OPERATOR 15 >= (uuid, uuid),
    OPERATOR 11 = (text, text),
    OPERATOR 12 < (text, text),
    OPERATOR 13 <= (text, text),
    OPERATOR 14 > (text, text),
    OPERATOR 15 >= (text, text),
    OPERATOR 11 = (int2, int4),
    OPERATOR 12 < (int2, int4),
    OPERATOR 13 <= (int2, int4),
    OPERATOR 14 > (int2, int4),
    OPERATOR 15 >= (int2, int4),
    OPERATOR 11 = (int2, int8),
    OPERATOR 12 < (int2, int8),
    OPERATOR 13 <= (int2, int8),
    OPERATOR 14 > (int2, int8),
    OPERATOR 15 >= (int2, int8),
    OPERATOR 11 = (int4, int2),
    OPERATOR 12 < (int4, int2),
    OPERATOR 13 <= (int4, int2),
    OPERATOR 14 > (int4, int2),
    OPERATOR 15 >= (int4, int2),
    OPERATOR 11 = (int4, int8),
    OPERATOR 12 < (int4, int8),
    OPERATOR 13 <= (int4, int8),
    OPERATOR 14 > (int4, int8),
    OPERATOR 15 >= (int4, int8),
    OPERATOR 11 = (int8, int2),
    OPERATOR 12 < (int8, int2),
    OPERATOR 13 <= (int8, int2),
    OPERATOR 14 > (int8, int2),
    OPERATOR 15 >= (int8, int2),
    OPERATOR 11 = (int8, int4),
    OPERATOR 12 < (int8, int4),
    OPERATOR 13 <= (int8, int4),
    OPERATOR 14 > (int8, int4),
    OPERATOR 15 >= (int8, int4),
    OPERATOR 11 = (float4, float8),
    OPERATOR 12 < (float4, float8),
    OPERATOR 13 <= (float4, float8),
    OPERATOR 14 > (float4, float8),
    OPERATOR 15 >= (float4, float8),
    OPERATOR 11 = (float8, float4),
    OPERATOR 12 < (float8, float4),
    OPERATOR 13 <= (float8, float4),
    OPERATOR 14 > (float8, float4),
    OPERATOR 15 >= (float8, float4);
