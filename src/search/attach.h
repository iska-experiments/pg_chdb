#ifndef CHDB_SEARCH_ATTACH_H
#define CHDB_SEARCH_ATTACH_H

/*
 * Putting an index's store table into the engine on demand (attach.c): the
 * engine's own metadata is a cache the worker starts without, and the
 * first request naming a generation attaches its table from the catalog.
 */

#include "postgres.h"

/*
 * Before a request of `index` is relayed: attaches the table of
 * `generation`, or of the index's current one when none is named, and its
 * staging tables, if the catalog's index names that generation and this
 * worker has not attached it yet. Raises with the engine's error if the
 * attach fails; for a generation the catalog does not have, does nothing,
 * and the engine answers the request with CHDB_STATUS_NO_STORE.
 */
extern void
chdb_search_attach(Oid index, uint64 generation);

#endif /* CHDB_SEARCH_ATTACH_H */
