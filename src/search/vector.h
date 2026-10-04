#ifndef CHDB_SEARCH_VECTOR_H
#define CHDB_SEARCH_VECTOR_H

/*
 * The vector columns of chdb_vector's operator classes, as vector.c serves
 * them to columns.c, ddl.c and query.c; the contract with the extension is
 * ../vector/chdb_vector.h.
 */

#include "postgres.h"

#include "access/skey.h"
#include "lib/stringinfo.h"
#include "utils/rel.h"

struct ChdbColumn;

/* Whether the column's class is one of chdb_vector's. */
extern bool
chdb_search_is_vector(Relation index, int attno);
/* `Array(Float32)`, once the column's dimension is checked. */
extern char*
chdb_search_vector_type(Relation index, int attno);
/* `vector_similarity(...)`, the TYPE of the column's skip index. */
extern char*
chdb_search_vector_index_type(Relation index, int attno);
/* The distance Postgres gets back for `col <op> q`. */
extern char*
chdb_search_vector_distance(
    Relation index,
    const struct ChdbColumn* col,
    ScanKey orderby
);
/* The ORDER BY, LIMIT and SETTINGS of a search by one distance operator. */
extern void
chdb_search_vector_order(
    Relation index,
    StringInfo buf,
    const struct ChdbColumn* col,
    ScanKey orderby,
    int64 limit
);

#endif /* CHDB_SEARCH_VECTOR_H */
