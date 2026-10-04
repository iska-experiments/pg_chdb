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
 * build, t_<generation>, named after the index's metapage generation, and a
 * meta table recording per generation the WAL position of the last flush
 * into it, which a backend compares with its metapage before trusting the
 * store. The engine makes the meta table with the database, so that the
 * comparison can be made of a store that has nothing else. Both tables keep
 * their parts on the index's callback object storage (CHDB_STORE_DISK_FMT),
 * whose blobs the worker holds; the meta DDL takes the index OID twice.
 */
#define CHDB_STORE_DB_FMT "idx_%" PRIu32
#define CHDB_STORE_TABLE_FMT CHDB_STORE_DB_FMT ".t_%" PRIu64
#define CHDB_STORE_META_FMT CHDB_STORE_DB_FMT ".meta"

/*
 * The callback object storage holding an index's blobs, which the engine
 * registers before a table is made on it (pagestore/protocol.h) and the
 * access method names in a table's SETTINGS, both from the index OID.
 */
#define CHDB_STORE_STORAGE_FMT "pg_%" PRIu32
#define CHDB_STORE_DISK_FMT                                                            \
    "disk = disk(type = 'callback', storage_name = '" CHDB_STORE_STORAGE_FMT "')"
#define CHDB_STORE_META_DDL                                                            \
    "CREATE TABLE IF NOT EXISTS " CHDB_STORE_META_FMT                                  \
    " (generation UInt64, lsn UInt64) ENGINE = ReplacingMergeTree(lsn) "               \
    "ORDER BY generation SETTINGS " CHDB_STORE_DISK_FMT

/*
 * The store directory under the data directory: a <dboid> subdirectory holding
 * each database's chDB store, and off Linux, where the worker's socket is
 * not abstract, the <dboid>.sock it listens on.
 */
#define CHDB_SEARCH_DIR "pg_chdb"
#define CHDB_SEARCH_SOCKET_FMT CHDB_SEARCH_DIR "/%u.sock"

/* A decoded request. The query borrows from the frame it was decoded from. */
typedef struct chdbSearchRequest {
    uint32_t index;        /* the index OID */
    uint64_t generation;   /* of the table the query works on, zero for none */
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
           chdb_setup_parse_head(&cur, &req->ctx, &req->query, &req->nparams);
}

#endif /* CHDB_SEARCH_PROTOCOL_H */
