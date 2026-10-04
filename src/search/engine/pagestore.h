#ifndef CHDB_SEARCH_ENGINE_PAGESTORE_H
#define CHDB_SEARCH_ENGINE_PAGESTORE_H

/*
 * The store tables' blobs, kept by the supervisor: libchdb's callback object
 * storage, one storage per index named CHDB_STORE_STORAGE_FMT, each callback
 * a page request to the supervisor (pagecall.h). The callbacks are
 * pagestore.c's, the registration pageregistry.c's; every function below says
 * why on stderr when it returns false.
 */

#include <stdbool.h>

#include "chdb.h"

#include "../pagestore/protocol.h"

/* A registered storage, the ud of its callbacks. */
typedef struct pageStorage {
    char name[CHDB_PAGE_STORAGE_MAX];
} pageStorage;

/* pagestore.c: fills the callback table serving `storage`. */
extern void
pagestore_callbacks(chdb_object_storage_callbacks* cb, pageStorage* storage);

/*
 * Checks that the libchdb loaded has the callback object storage and starts
 * the reader of the supervisor's replies on the page socket `fd`.
 */
extern bool
pagestore_start(int fd);

/*
 * Registers every storage the supervisor holds blobs for. Before the store is
 * opened: libchdb attaches a table to its storage by name as it loads the
 * metadata, and refuses a name it does not know.
 */
extern bool
pagestore_bootstrap(void);

/* Registers the storage `name` unless it is registered; before a table is made on it.
 */
extern bool
pagestore_register(const char* name);

#endif /* CHDB_SEARCH_ENGINE_PAGESTORE_H */
