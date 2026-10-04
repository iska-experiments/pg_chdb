-- Appended to sql/chdb_search.sql by the Makefile, after chdb.query.
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
CREATE OPERATOR @@/ (
    LEFTARG = text, RIGHTARG = text, FUNCTION = chdb.regex,
    RESTRICT = chdb_search_restrict_sel, JOIN = chdb_search_join_sel
);
CREATE OPERATOR @@% (
    LEFTARG = text, RIGHTARG = text, FUNCTION = chdb.wildcard,
    RESTRICT = chdb_search_restrict_sel, JOIN = chdb_search_join_sel
);
CREATE OPERATOR @@@ (
    LEFTARG = text, RIGHTARG = chdb.query, FUNCTION = chdb.query_matches,
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
CREATE OPERATOR @@@ (
    LEFTARG = text[], RIGHTARG = chdb.query, FUNCTION = chdb.query_matches,
    RESTRICT = chdb_search_restrict_sel, JOIN = chdb_search_join_sel
);

----------------------------------------------------------------------------
-- Operator classes. Strategies: 1 has_all_tokens @@@, 2 has_any_tokens @@?,
-- 3 has_token @@=, 4 has_phrase @@~ (needs support_phrase_search), 5 a
-- chdb.query tree @@@, 6 regex @@/, 7 wildcard @@%.
----------------------------------------------------------------------------
CREATE OPERATOR CLASS text_ops
DEFAULT FOR TYPE text USING chdb AS
    OPERATOR 1 @@@ (text, text),
    OPERATOR 2 @@? (text, text),
    OPERATOR 3 @@= (text, text),
    OPERATOR 4 @@~ (text, text),
    OPERATOR 5 @@@ (text, chdb.query),
    OPERATOR 6 @@/ (text, text),
    OPERATOR 7 @@% (text, text),
    FUNCTION 1 (text) chdb_search_text_options(internal);

-- The default for text[], or columnar_ops would take it through anyelement
-- and store an array nothing can search.
CREATE OPERATOR CLASS text_array_ops
DEFAULT FOR TYPE text[] USING chdb AS
    OPERATOR 1 @@@ (text[], text),
    OPERATOR 2 @@? (text[], text),
    OPERATOR 3 @@= (text[], text),
    OPERATOR 5 @@@ (text[], chdb.query),
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
