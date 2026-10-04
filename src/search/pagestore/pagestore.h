#ifndef CHDB_SEARCH_PAGESTORE_H
#define CHDB_SEARCH_PAGESTORE_H

/*
 * The worker's side of the page request protocol (protocol.h): answering
 * the engine's storage callbacks from the blobs in index relation pages
 * (pagebackend.c behind store.h). The worker calls chdb_pagestore_serve
 * whenever the engine's page channel is readable: from its event loop while
 * idle, from inside any wait on the engine's request channel while
 * relaying (channel.h's aside), and while waiting for the engine to stop.
 * None of those run in a transaction, so the pins and locks the pages take
 * are owned by a resource owner of the store's own, released on an error.
 */

#include "postgres.h"

#include "access/xlogdefs.h"
#include "utils/rel.h"

#include "../../channel.h"
#include "store.h"

/* Once per worker, before the engine starts. */
extern void
chdb_pagestore_init(void);

/*
 * A request named `index` and the relation at `loc` as its backend sees it:
 * the blobs of the generation the relation's metapage holds go there. A
 * relation seen for the first time with pages a crashed worker took is
 * recovered first (recover.c).
 */
extern void
chdb_pagestore_note(Oid index, RelFileLocator loc);

/* Before the index's database is dropped: its blobs go with its relations. */
extern void
chdb_pagestore_forget(Oid index);

/*
 * After promotion: forgets every relation, so that each is noted afresh
 * when next named, and recovered then if a crashed primary left it dirty.
 */
extern void
chdb_pagestore_reset(void);

/*
 * A version of the blobs in the relation at `loc`: the latest LSN among its
 * metapage and directory pages, which every blob put or removed moves, on
 * a standby by replay. InvalidXLogRecPtr for a relation gone or empty.
 */
extern XLogRecPtr
chdb_pagestore_version(RelFileLocator loc);

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

/* The blobs of a storage under `prefix`, for the worker's own use. */
extern void
chdb_pagestore_list(
    const char* storage,
    const char* prefix,
    chdbBlobListSink sink,
    void* ud
);

/* The blobs in an index relation's pages, from a backend that has it open. */
extern void
chdb_pagestore_list_relation(Relation index, chdbBlobListSink sink, void* ud);

/* Whether the index relation holds any blob under `prefix`, likewise. */
extern bool
chdb_pagestore_has_blobs(Relation index, const char* prefix);

/* After the engine's last stop: marks every relation written as clean. */
extern void
chdb_pagestore_shutdown(void);

#endif /* CHDB_SEARCH_PAGESTORE_H */
