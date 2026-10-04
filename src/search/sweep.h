#ifndef CHDB_SEARCH_SWEEP_H
#define CHDB_SEARCH_SWEEP_H

/*
 * The worker's startup sweep: the engine's directory emptied, the
 * directories of dropped databases removed.
 */

#include "postgres.h"

/*
 * Empties the engine's directory and removes the directories of databases
 * that no longer exist. For the worker of database `dboid`, before it
 * listens. Raises on failure.
 */
extern void
chdb_search_sweep(Oid dboid);

/* Removes a database's engine directory and socket under pg_chdb/pgsql_tmp. */
extern void
chdb_search_remove_store_dir(Oid dboid);

/* Removes the engine directory alone, for a worker that keeps listening. */
extern void
chdb_search_empty_engine_dir(Oid dboid);

/* Whether the catalog has `relid` as an index of the chdb access method. */
extern bool
chdb_search_is_index(Oid relid);

#endif /* CHDB_SEARCH_SWEEP_H */
