#ifndef CHDB_SEARCH_PROTOCOL_H
#define CHDB_SEARCH_PROTOCOL_H

/* Plain C: the engine program includes this without Postgres. */
#include "../setup.h"

/*
 * The wire protocol between backends and the chdb_search worker, spoken over
 * the unix stream socket the worker listens on (serve.c), and between the
 * worker and its chdb_search_engine child over a socketpair.
 * Fields are native endian. A string is a uint32 byte count followed by that
 * many bytes, unterminated.
 *
 * A connection carries one request at a time and can carry many in turn.
 *
 *   request    uint32  byte count of the rest of the request, at most
 *                      CHDB_SETUP_MAX
 *              uint32  index OID
 *              uint64  store generation: the metapage generation of the
 *                      table the statement works on, which the engine checks
 *                      exists before running it; zero for a statement that
 *                      creates that table, works in no table, or cleans up
 *                      after an abort
 *              uint32  tablespace OID and
 *              uint32  relfilenumber of the index relation as the backend
 *                      sees it, which the worker holds the index's blobs in
 *                      (pagestore/routes.h): a build's relation is in no
 *                      catalog the worker can read, so the backend says.
 *                      Zero when the backend has no relation to name, as
 *                      a drop after commit has not.
 *              then the setup payload of src/setup.h verbatim, as
 *              chdb_helper_build_setup writes it for chdb_helper: command,
 *              limits, query and parameters. The command is CHDB_CMD_SELECT,
 *              _INSERT, _EXEC or _DROP; parameters are not supported yet.
 *   data       Native blocks as a run of chunks, each a uint32 byte count of
 *              at most CHDB_CHUNK_MAX and that many bytes, ended by a zero
 *              count. A chunk is an arbitrary slice of the stream, not a
 *              block. This is the chunked form of channel.h.
 *                CHDB_CMD_INSERT   client to worker, after the request
 *                CHDB_CMD_SELECT   worker to client, after the request
 *                EXEC, DROP        none
 *   status     worker to client, last frame of every request:
 *                uint8   CHDB_STATUS_OK, _ERROR, or _NO_STORE when the
 *                        request's generation names a table the store does
 *                        not have, so the index needs a REINDEX
 *                string  error text as chDB said it, empty on success
 *                        (a debug command's answer)
 *
 * An INSERT the worker cannot run is still read to its end-of-data chunk so
 * the connection stays in step; the failure comes back in the status. A SELECT
 * that fails after sending data ends its data early and reports in the status.
 * The worker closes the connection only when it cannot trust the framing.
 *
 * The worker passes these frames to and from its engine, which speaks the
 * same protocol, without reading the data between. If the engine dies, the
 * worker ends any data the client is owed and reports the death in the status,
 * so the client sees an error for that request and the connection goes on.
 */

/* Worker-handled commands, continuing the numbering of src/setup.h. */
#define CHDB_CMD_EXEC 'E' /* run a statement, no result rows */
#define CHDB_CMD_DROP 'X' /* drop the index's chDB database */

/*
 * Debug commands the worker answers itself. The reply is a success status
 * whose string is the engine's pid, zero when none runs. The query of KILL is
 * the signal number to send to the engine, which must be running.
 */
#define CHDB_CMD_ENGINE_PID 'P'
#define CHDB_CMD_ENGINE_KILL 'K'

/* The status byte that ends every request. */
#define CHDB_STATUS_OK 0
#define CHDB_STATUS_ERROR 1
#define CHDB_STATUS_NO_STORE 2

/*
 * The layout of a store, which the access method writes and the engine
 * checks: one chDB database per index, idx_<oid>, holding one table per
 * build, t_<generation>, named after the index's metapage generation and
 * given a UUID fixed by the two (ddl.c), so that a worker can attach what
 * the index relation's pages hold without the engine's metadata. The
 * table keeps its parts on the index's callback object storage
 * (CHDB_STORE_DISK_FMT), whose blobs the worker holds, under a key prefix
 * naming the generation, so that the worker tells the blobs of a rebuild
 * being written beside the generation it still serves.
 */
#define CHDB_STORE_DB_FMT "idx_%" PRIu32
#define CHDB_STORE_TABLE_FMT CHDB_STORE_DB_FMT ".t_%" PRIu64

/*
 * The callback object storage holding an index's blobs, which the engine
 * registers before a table is made on it (pagestore/protocol.h) and the
 * access method names in a table's SETTINGS, both from the index OID. Each
 * table has a key prefix of its own on the storage, as libchdb asks of
 * disks sharing one: the generation for a build's table, the generation
 * and the transaction for a staging table (staging.c), so that the worker
 * can tell a generation's blobs apart and find its staging tables again.
 * The disk format takes the index OID and the rendered prefix.
 */
#define CHDB_STORE_STORAGE_FMT "pg_%" PRIu32
#define CHDB_STORE_KEY_PREFIX_FMT "g%" PRIu64
#define CHDB_STORE_STAGING_PREFIX_FMT "s%" PRIu64 "_tx_%" PRIu64
#define CHDB_STORE_DISK_FMT                                                            \
    "disk = disk(type = 'callback', storage_name = '" CHDB_STORE_STORAGE_FMT           \
    "', key_prefix = '%s')"

/*
 * A standby's engine names its disk, from the index OID and the generation,
 * so that the worker can have the disk read the pages afresh by name
 * (SYSTEM RESTART DISK) before it puts the table back (attach.c); the
 * primary's disk takes the name ClickHouse makes of its definition. The
 * read-only disk format takes the index OID and generation, then the index
 * OID and the rendered prefix as the other does.
 */
#define CHDB_STORE_DISK_NAME_FMT "pg_%" PRIu32 "_g%" PRIu64
#define CHDB_STORE_DISK_RO_FMT                                                         \
    "disk = disk(name = '" CHDB_STORE_DISK_NAME_FMT                                    \
    "', type = 'callback', storage_name = '" CHDB_STORE_STORAGE_FMT                    \
    "', key_prefix = '%s')"

/*
 * The worker's directory under the data directory: pg_chdb/pgsql_tmp holds a
 * <dboid> subdirectory for each database's engine, chDB's own metadata and
 * scratch space, and off Linux, where the worker's socket is not abstract,
 * the <dboid>.sock it listens on. The engine's directory is a cache the
 * worker empties when it starts and refills from the catalog and the index
 * pages (sweep.c, attach.c), so it is named for Postgres to leave out of
 * base backups and pg_rewind, as it leaves out every pgsql_tmp.
 */
#define CHDB_SEARCH_DIR "pg_chdb"
#define CHDB_SEARCH_CACHE_DIR CHDB_SEARCH_DIR "/pgsql_tmp"
#define CHDB_SEARCH_ENGINE_DIR_FMT CHDB_SEARCH_CACHE_DIR "/%u"
#define CHDB_SEARCH_SOCKET_FMT CHDB_SEARCH_CACHE_DIR "/%u.sock"

/*
 * The engine's optional last argument: the worker runs on a server in
 * recovery, whose pages it cannot write, so the engine refuses every
 * callback that would write, attaches its tables read-only and runs no
 * merges (engine/readonly.c). Promotion restarts the engine without it.
 */
#define CHDB_SEARCH_ENGINE_READONLY "readonly"

/* A decoded request. The query borrows from the frame it was decoded from. */
typedef struct chdbSearchRequest {
    uint32_t index;      /* the index OID */
    uint64_t generation; /* of the table the query works on, zero for none */
    uint32_t tablespace; /* the index relation's locator, zero for none */
    uint32_t relnumber;
    chdbHelperContext ctx; /* command and limits */
    chdbSetupStr query;
    uint16_t nparams;
} chdbSearchRequest;

/* Decodes the `len` bytes of a request after its byte count. False if they end early.
 */
static inline bool
chdb_search_decode_request(const char* frame, size_t len, chdbSearchRequest* req) {
    chdbSetupCursor cur = { .at = frame, .end = frame + len };

    return chdb_setup_take(&cur, &req->index, sizeof(req->index)) &&
           chdb_setup_take(&cur, &req->generation, sizeof(req->generation)) &&
           chdb_setup_take(&cur, &req->tablespace, sizeof(req->tablespace)) &&
           chdb_setup_take(&cur, &req->relnumber, sizeof(req->relnumber)) &&
           chdb_setup_parse_head(&cur, &req->ctx, &req->query, &req->nparams);
}

#endif /* CHDB_SEARCH_PROTOCOL_H */
