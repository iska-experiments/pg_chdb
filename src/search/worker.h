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

#endif /* CHDB_SEARCH_WORKER_H */
