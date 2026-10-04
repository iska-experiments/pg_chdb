-- Appended to sql/chdb_search.sql by the Makefile, before the operators.
----------------------------------------------------------------------------
-- chdb.query: a search as a tree, searched for with `col @@@ query`
----------------------------------------------------------------------------
-- The leaves are the searches of the operators above, built by the
-- functions of the same names below; boost weights a leaf for the score and
-- leaves the filter alone; && || and ! (or all_of, any_of and none_of)
-- combine. The readable form, which EXPLAIN shows, writes each node as the
-- call that builds it: and(match_all('a b'), not(phrase('c d', 1))).
CREATE TYPE chdb.query;

CREATE FUNCTION chdb.query_in(cstring)
RETURNS chdb.query
AS 'MODULE_PATHNAME', 'chdb_search_query_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION chdb.query_out(chdb.query)
RETURNS cstring
AS 'MODULE_PATHNAME', 'chdb_search_query_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE chdb.query (
    INPUT = chdb.query_in, OUTPUT = chdb.query_out,
    INTERNALLENGTH = VARIABLE, STORAGE = extended
);

CREATE FUNCTION chdb.match(text) RETURNS chdb.query
AS 'MODULE_PATHNAME', 'chdb_search_q_match_any'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION chdb.match_all(text) RETURNS chdb.query
AS 'MODULE_PATHNAME', 'chdb_search_q_match_all'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION chdb.term(text) RETURNS chdb.query
AS 'MODULE_PATHNAME', 'chdb_search_q_term'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
-- slop: how many other tokens may come between the needle's, kept in order.
CREATE FUNCTION chdb.phrase(text, slop integer DEFAULT 0) RETURNS chdb.query
AS 'MODULE_PATHNAME', 'chdb_search_q_phrase'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION chdb.regex(text) RETURNS chdb.query
AS 'MODULE_PATHNAME', 'chdb_search_q_regex'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION chdb.wildcard(text) RETURNS chdb.query
AS 'MODULE_PATHNAME', 'chdb_search_q_wildcard'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION chdb.boost(chdb.query, real) RETURNS chdb.query
AS 'MODULE_PATHNAME', 'chdb_search_q_boost'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
-- Names the index column the leaves search, for a tree over several
-- columns. Such a query is answered through the index only.
CREATE FUNCTION chdb.in_column(chdb.query, text) RETURNS chdb.query
AS 'MODULE_PATHNAME', 'chdb_search_q_in_column'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION chdb.query_and(chdb.query, chdb.query) RETURNS chdb.query
AS 'MODULE_PATHNAME', 'chdb_search_q_and'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION chdb.query_or(chdb.query, chdb.query) RETURNS chdb.query
AS 'MODULE_PATHNAME', 'chdb_search_q_or'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION chdb.none_of(chdb.query) RETURNS chdb.query
AS 'MODULE_PATHNAME', 'chdb_search_q_not'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION chdb.all_of(VARIADIC chdb.query[]) RETURNS chdb.query
AS 'MODULE_PATHNAME', 'chdb_search_q_all_of'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION chdb.any_of(VARIADIC chdb.query[]) RETURNS chdb.query
AS 'MODULE_PATHNAME', 'chdb_search_q_any_of'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR && (LEFTARG = chdb.query, RIGHTARG = chdb.query, FUNCTION = chdb.query_and);
CREATE OPERATOR || (LEFTARG = chdb.query, RIGHTARG = chdb.query, FUNCTION = chdb.query_or);
CREATE OPERATOR ! (RIGHTARG = chdb.query, FUNCTION = chdb.none_of);

-- The Postgres implementation of `col @@@ query` evaluates the tree with
-- the fallbacks of the leaves' operators. The array form is not strict: a
-- NULL array reads as empty, as the index stores it, so that a NOT answers
-- alike both ways.
CREATE FUNCTION chdb.query_matches(text, chdb.query)
RETURNS boolean
AS 'MODULE_PATHNAME', 'chdb_search_query_matches'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 10;

CREATE FUNCTION chdb.query_matches(text[], chdb.query)
RETURNS boolean
AS 'MODULE_PATHNAME', 'chdb_search_array_query_matches'
LANGUAGE C IMMUTABLE PARALLEL SAFE COST 10;
