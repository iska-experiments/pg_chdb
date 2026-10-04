#ifndef CHDB_SEARCH_WORKER_H
#define CHDB_SEARCH_WORKER_H

#include "postgres.h"

/* Settings handed to chDB, defined by the CHDB_GUCS("chdb_search") GUCs. */
extern int chdb_max_memory;
extern int chdb_max_threads;
extern int chdb_max_parsers;

/* Where the worker finds libchdb, as dlopen takes it. */
extern char* chdb_search_libchdb_path;

/* Seconds a client waits for a worker to come up. */
extern int chdb_search_worker_timeout;

/*
 * Makes sure a worker is running or starting for database `dboid`. Cheap when
 * one is. Registers a worker if none is, which the caller then waits for by
 * trying the socket. Raises if the worker cannot be registered.
 */
extern void
chdb_search_worker_ensure(Oid dboid);

/* Entry point the postmaster calls in the worker process. */
extern PGDLLEXPORT void
chdb_search_worker_main(Datum arg);

#endif /* CHDB_SEARCH_WORKER_H */
