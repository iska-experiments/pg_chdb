#ifndef CHDB_SEARCH_PAGESTORE_PROTOCOL_H
#define CHDB_SEARCH_PAGESTORE_PROTOCOL_H

/* Plain C: the engine program includes this without Postgres. */
#include "../../setup.h"

/*
 * The page request protocol between the chdb_search_engine and its
 * supervisor, the chdb_search worker, spoken over the second socketpair the
 * worker hands the engine (CHDB_SEARCH_PAGE_FD). The engine keeps its store
 * tables on libchdb's callback object storage, whose callbacks it answers by
 * asking the worker, the process that owns the blobs: in this phase as files
 * under the data directory, later as pages of the index relation. Fields are
 * native endian. A string is a uint32 byte count followed by that many bytes,
 * unterminated. A handle is a uint64 the worker chose.
 *
 * The callbacks run on any thread of the engine, several at once, and while
 * the thread inside libchdb is blocked, so the socket carries many requests
 * in flight: every request names an id, and the reply repeats it. The engine
 * writes each frame whole under a mutex and a reader thread of its own
 * matches replies to the waiting callback by id. The worker answers them in
 * the order received, from its event loop while it is idle and from inside
 * any wait on the engine's other socket while it is relaying a request, so
 * that a callback the engine blocked on never waits for a reply the worker
 * would only send once the engine answered.
 *
 *   request    engine to worker
 *              uint32  id
 *              uint32  op, one of CHDB_PAGE_*
 *              uint32  byte count of the body, at most CHDB_PAGE_BODY_MAX
 *              body    by op:
 *                STORAGES      nothing: the names of the storages the worker
 *                              holds blobs for, which the engine registers
 *                              before it opens the store, since libchdb
 *                              attaches a table to its storage by name
 *                EXISTS        string storage, string key
 *                METADATA      string storage, string key
 *                READ          string storage, string key, uint64 offset,
 *                              uint32 bytes wanted
 *                WRITE_BEGIN   string storage, string key
 *                WRITE_APPEND  uint64 handle, then the bytes to append, to
 *                              the end of the body
 *                WRITE_COMMIT  uint64 handle
 *                WRITE_ABORT   uint64 handle
 *                REMOVE        string storage, string key
 *                LIST          string storage, string prefix
 *                COPY          string storage, string from key, string to key
 *   reply      worker to engine
 *              uint32  id of the request answered
 *              uint32  status: CHDB_PAGE_OK, or CHDB_PAGE_ERROR with the
 *                      body holding the error text
 *              uint32  byte count of the body, at most CHDB_PAGE_BODY_MAX
 *              body    on success, by op:
 *                STORAGES      string name, repeated
 *                EXISTS        uint8 found
 *                METADATA      uint8 found; if found, uint64 size and int64
 *                              mtime, Unix seconds of the commit
 *                READ          the bytes: min(wanted, size - offset) of them
 *                WRITE_BEGIN   uint64 handle
 *                LIST          string key, uint64 size, int64 mtime, repeated
 *                others        nothing
 *
 * A blob is visible from the reply to its WRITE_COMMIT on; a failed commit
 * drops the pending blob and the handle with it, as libchdb's contract says
 * no abort follows. An engine that dies leaves its handles for the worker to
 * drop. A frame that cannot be read to its end, on either side, ends the
 * engine: the worker kills and reaps it, the engine exits.
 */

/* The descriptor the engine is handed its end of the page socketpair as. */
#define CHDB_SEARCH_PAGE_FD 4

#define CHDB_PAGE_STORAGES 1
#define CHDB_PAGE_EXISTS 2
#define CHDB_PAGE_METADATA 3
#define CHDB_PAGE_READ 4
#define CHDB_PAGE_WRITE_BEGIN 5
#define CHDB_PAGE_WRITE_APPEND 6
#define CHDB_PAGE_WRITE_COMMIT 7
#define CHDB_PAGE_WRITE_ABORT 8
#define CHDB_PAGE_REMOVE 9
#define CHDB_PAGE_LIST 10
#define CHDB_PAGE_COPY 11

#define CHDB_PAGE_OK 0
#define CHDB_PAGE_ERROR 1

/* Room for a chunk of data and the fields around it; a corrupt count is refused. */
#define CHDB_PAGE_BODY_MAX (CHDB_CHUNK_MAX + 64)

/* The engine splits reads and appends past this into several requests. */
#define CHDB_PAGE_DATA_MAX CHDB_CHUNK_MAX

/* The fixed heads of the two frames, all 32-bit so that they pack as written. */
typedef struct chdbPageRequest {
    uint32_t id;
    uint32_t op;
    uint32_t len;
} chdbPageRequest;

typedef struct chdbPageReply {
    uint32_t id;
    uint32_t status;
    uint32_t len;
} chdbPageReply;

/* A storage name: alphanumerics and underscores, shorter than this. */
#define CHDB_PAGE_STORAGE_MAX 64

#endif /* CHDB_SEARCH_PAGESTORE_PROTOCOL_H */
