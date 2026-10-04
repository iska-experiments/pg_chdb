#ifndef CHDB_SEARCH_PAGESTORE_H
#define CHDB_SEARCH_PAGESTORE_H

/*
 * The worker's side of the page request protocol (protocol.h): answering
 * the engine's storage callbacks from the blob store (store.h). The worker
 * calls chdb_pagestore_serve whenever the engine's page channel is readable:
 * from its event loop while idle, from inside any wait on the engine's
 * request channel while relaying (channel.h's aside), and while waiting for
 * the engine to stop.
 */

#include "postgres.h"

#include "../../channel.h"
#include "store.h"

/* Once per worker, before the engine starts: the backend's handles and its sweep. */
extern void
chdb_pagestore_init(void);

/*
 * Reads one request from the page channel and answers it. A failure of the
 * backend becomes an error reply; a channel that breaks, or a frame that
 * cannot be trusted, raises for the caller to put the engine down.
 */
extern void
chdb_pagestore_serve(chdbChannel* page);

/* Drops the pending writes of an engine that is gone. */
extern void
chdb_pagestore_engine_gone(void);

/* The blobs of a storage, from any process of the database: the debug function's. */
extern void
chdb_pagestore_list(const char* storage, chdbBlobListSink sink, void* ud);

#endif /* CHDB_SEARCH_PAGESTORE_H */
