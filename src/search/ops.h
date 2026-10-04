#ifndef CHDB_SEARCH_OPS_H
#define CHDB_SEARCH_OPS_H

/*
 * The Postgres implementations of the search predicates (ops.c for the
 * token searches, pattern.c for regex and wildcard), which a sequential
 * scan, a recheck and the chdb.query fallback (queryeval.c) evaluate, and
 * which validate.c knows the query operators by.
 */

#include "postgres.h"

#include "fmgr.h"
#include "utils/array.h"

/* Lowercases as the index's lowerUTF8 does; palloc'd, NUL-terminated. */
extern char*
chdb_search_lower(const char* s, size_t n);
extern bool
chdb_search_regex_matches(text* hay, text* re, Oid collation);
extern bool
chdb_search_wildcard_matches(text* hay, text* pattern);
/*
 * A text search by strategy (query.h), as the operator of that strategy
 * answers it: of a text, with a phrase's slop, or of a text[], whose
 * elements are tokens and which no phrase or pattern applies to.
 */
extern bool
chdb_search_text_leaf(int strategy, text* hay, text* needle, int32 slop, Oid collation);
extern bool
chdb_search_array_leaf(int strategy, ArrayType* arr, text* needle);
/* Raises for a strategy a text[] column has no answer to. */
extern void
chdb_search_check_array_search(int strategy);
/* The operator functions of `col @@@ query`, which validate.c knows by address. */
extern PGDLLEXPORT Datum chdb_search_query_matches(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum chdb_search_array_query_matches(PG_FUNCTION_ARGS);

#endif /* CHDB_SEARCH_OPS_H */
