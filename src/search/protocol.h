#ifndef CHDB_SEARCH_PROTOCOL_H
#define CHDB_SEARCH_PROTOCOL_H

/* Plain C: the engine program includes this without Postgres. */
#include <stdint.h>

#include "../setup.h"

/*
 * The wire protocol between backends and the chdb_search worker, spoken over
 * the unix stream socket at pg_chdb/<dboid>.sock under the data directory.
 * Fields are native endian. A string is a uint32 byte count followed by that
 * many bytes, unterminated.
 *
 * A connection carries one request at a time and can carry many in turn.
 *
 *   request    the setup payload of src/setup.h with the index OID added:
 *                uint8   command: CHDB_CMD_SELECT, _INSERT, _EXEC or _DROP
 *                uint32  index OID
 *                uint16  max_memory, max_threads, max_parsers, as in setup.h
 *                string  query
 *                uint16  parameter count, always zero for now
 *   data       Native blocks as a run of chunks, each a uint32 byte count
 *              and that many bytes, ended by a chunk of zero bytes. A chunk
 *              is an arbitrary slice of the stream, not a block boundary.
 *                CHDB_CMD_INSERT   client to worker, after the request
 *                CHDB_CMD_SELECT   worker to client, after the request
 *                EXEC, DROP        none
 *   status     worker to client, last frame of every request:
 *                uint8   zero for success
 *                string  error text, empty on success (a debug command's answer)
 *
 * An INSERT the worker cannot run is still read to its end-of-data chunk so
 * the connection stays in step; the failure comes back in the status. A SELECT
 * that fails after sending data ends its data early and reports in the status.
 * The worker closes the connection only when it cannot trust the framing.
 *
 * The worker passes these frames to and from its chdb_search_engine child,
 * which speaks the same protocol on a socketpair. If the engine dies, the
 * worker ends any data the client is owed and reports the death in the status,
 * so the client sees an error for that request and the connection goes on.
 */

/* Worker-handled command, continuing the numbering of src/setup.h. */
#define CHDB_CMD_EXEC 'E' /* run a statement, no result rows */
#define CHDB_CMD_DROP 'X' /* drop the index's chDB database */

/*
 * Debug commands the worker answers itself. The reply is a success status
 * whose string is the engine's pid, zero when none runs. The query of KILL is
 * the signal number to send to the engine, which must be running.
 */
#define CHDB_CMD_ENGINE_PID 'P'
#define CHDB_CMD_ENGINE_KILL 'K'

/* Largest chunk either side will take, so a corrupt count cannot size a buffer. */
#define CHDB_SEARCH_CHUNK_MAX (8 * 1024 * 1024)

/*
 * The store directory under the data directory: a <dboid> subdirectory holding
 * each database's chDB store, and the <dboid>.sock its worker listens on.
 */
#define CHDB_SEARCH_DIR "pg_chdb"
#define CHDB_SEARCH_SOCKET_FMT CHDB_SEARCH_DIR "/%u.sock"

#endif /* CHDB_SEARCH_PROTOCOL_H */
