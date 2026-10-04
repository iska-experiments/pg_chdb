#ifndef CHDB_SEARCH_CLIENT_H
#define CHDB_SEARCH_CLIENT_H

/*
 * Backend-side client for the per-database chdb_search worker.
 *
 * The worker owns the chDB store for the current database (chDB lets only
 * one process open a store path). Backends never link libchdb; they send
 * requests over a unix socket at $PGDATA/pg_chdb/<dboid>.sock and stream
 * Native blocks both ways. The client starts the worker on first use.
 *
 * Every request names the index it works on. Before running it the worker
 * creates the chDB database `idx_<indexoid>` if it is missing. Queries pass
 * through unchanged, so callers name the table in full: `idx_<indexoid>.t`.
 * The framing is documented in protocol.h.
 */

#include "postgres.h"

/* Commands: CHDB_CMD_SELECT and _INSERT of src/setup.h, plus _EXEC and _DROP. */
#include "protocol.h"

typedef struct chdbSearchConn chdbSearchConn;

/* Opens a connection to the worker, starting it if needed. Never NULL. */
extern chdbSearchConn*
chdb_search_connect(void);

/* Closes the connection. Safe on an already-closed connection. */
extern void
chdb_search_close(chdbSearchConn* conn);

/*
 * Runs `sql`, which names the table as `idx_<indexoid>.t`.
 * Raises on failure with the worker's error text.
 */
extern void
chdb_search_exec(chdbSearchConn* conn, Oid indexoid, const char* sql);

/*
 * Runs `sql`, whose Native blocks the caller reads from chdb_search_channel
 * until it returns zero, then calls chdb_search_finish.
 */
extern void
chdb_search_select(chdbSearchConn* conn, Oid indexoid, const char* sql);

/*
 * Starts `sql`, an `INSERT INTO idx_<indexoid>.t (cols)` with no FORMAT
 * clause, which the worker supplies as Native; the caller then sends blocks
 * through chdb_search_channel and ends the stream with chdb_search_finish,
 * which waits for the worker's acknowledgement and raises on failure.
 */
extern void
chdb_search_insert(chdbSearchConn* conn, Oid indexoid, const char* sql);

/*
 * Ends the request under way and reads its status, raising with the worker's
 * error text when it failed. A select that ClickHouse rejected has no data,
 * so this is where a caller that read none finds out.
 */
extern void
chdb_search_finish(chdbSearchConn* conn);

/* Drops the index's chDB database. Idempotent. */
extern void
chdb_search_drop(chdbSearchConn* conn, Oid indexoid);

/*
 * The channel of the connection, for the Native streaming code of native.h.
 * Use after chdb_search_select or chdb_search_insert; finish the stream with
 * chdb_search_finish.
 */
extern chdbChannel*
chdb_search_channel(chdbSearchConn* conn);

#endif /* CHDB_SEARCH_CLIENT_H */
