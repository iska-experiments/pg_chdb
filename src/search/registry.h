#ifndef CHDB_SEARCH_REGISTRY_H
#define CHDB_SEARCH_REGISTRY_H

/*
 * The registry of chdb_search workers in shared memory, one slot per
 * database, so that only one backend registers a database's worker.
 */

#include "postgres.h"

#include "storage/spin.h"
#include "utils/timestamp.h"

#define CHDB_SEARCH_MAX_WORKERS 64

typedef enum workerState {
    WORKER_FREE = 0,
    WORKER_STARTING, /* a backend registered it; it has not claimed the slot yet */
    WORKER_RUNNING,
} workerState;

typedef struct workerSlot {
    Oid dboid;
    pid_t pid; /* the worker once RUNNING */
    workerState state;
    TimestampTz since; /* when STARTING began, so a lost start can be retried */
} workerSlot;

typedef struct workerRegistry {
    slock_t lock;
    workerSlot slots[CHDB_SEARCH_MAX_WORKERS];
} workerRegistry;

extern workerRegistry*
chdb_search_registry(void);

/* The slot for `dboid`, else a free one, else NULL. Call with the lock held. */
extern workerSlot*
chdb_search_find_slot(workerRegistry* reg, Oid dboid);

/* A crashed worker leaves its slot RUNNING, so trust the process table. */
extern bool
chdb_search_pid_alive(pid_t pid);

/* Frees the slot for `dboid`; with `only_pid` set, only if that worker holds it. */
extern void
chdb_search_free_slot(Oid dboid, pid_t only_pid);

#endif /* CHDB_SEARCH_REGISTRY_H */
