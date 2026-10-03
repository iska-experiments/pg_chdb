-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION chdb_search" to load this file. \quit

CREATE FUNCTION chdb_search_version() RETURNS TEXT
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
