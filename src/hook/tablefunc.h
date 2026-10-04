#ifndef CHDB_TABLEFUNC_H
#define CHDB_TABLEFUNC_H

#include "postgres.h"

#include "lib/stringinfo.h"

#include "copy.h"

/*
 * Appends to `query` the chDB statement of `ctx`'s copy: a SELECT from, an
 * INSERT into or a DESCRIBE of the table function its URL scheme names, with
 * the settings the command wants. The URL, credentials, format, structure and
 * compression travel as query parameters, filled into `names` and `values`,
 * each sized CHDB_MAX_TABLEFUNC_ARGS; returns how many. Logs the query and
 * its parameters at DEBUG1, for the TAP tests.
 */
extern size_t
chdb_table_function_query(
    chdbCopyContext* ctx,
    StringInfo query,
    char** names,
    char** values
);

#endif /* CHDB_TABLEFUNC_H */
