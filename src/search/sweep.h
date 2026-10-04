#ifndef CHDB_SEARCH_SWEEP_H
#define CHDB_SEARCH_SWEEP_H

/*
 * The worker's startup sweep of what a drop left behind, and the removal of
 * a dropped database's store directory, which drop.c also runs at commit.
 */

#include "postgres.h"

/*
 * Drops the idx_<oid> stores of indexes that no longer exist and removes the
 * directories of databases that no longer exist. For the worker of database
 * `dboid`, before it listens. Raises on failure, with the engine stopped.
 */
extern void
chdb_search_sweep(Oid dboid);

/* Removes pg_chdb/<dboid> and its socket, the local store of a database. */
extern void
chdb_search_remove_store_dir(Oid dboid);

#endif /* CHDB_SEARCH_SWEEP_H */
