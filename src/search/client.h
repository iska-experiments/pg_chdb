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
 * Every request names the index it works on and the store generation of the
 * table it works on. Before running it the worker creates the chDB database
 * `idx_<indexoid>` if it is missing and, for a non-zero generation, checks
 * that `idx_<indexoid>.t_<generation>` exists, raising with a REINDEX hint if
 * not, so a statement against a store that is gone or rebuilt fails clearly
 * rather than in ClickHouse's words. Queries pass through unchanged, so
 * callers name the table in full. The framing is documented in protocol.h.
 *
 * Every call returns or raises within chdb_search.worker_timeout when
 * interrupts are held (a commit or abort callback), where a cancel could not
 * end a wait for a worker that is busy or stopped; elsewhere waits are
 * interruptible and unbounded.
 */

#include "postgres.h"

#include "../channel.h"
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
 * Runs `sql`, which names the table as `idx_<indexoid>.t_<generation>`.
 * `generation` is that table's, or zero for a statement that creates it,
 * works in no table, or cleans up after an abort. Raises on failure with the
 * worker's error text.
 */
extern void
chdb_search_exec(
    chdbSearchConn* conn,
    Oid indexoid,
    uint64 generation,
    const char* sql
);

/*
 * Runs `sql`, whose Native blocks the caller reads from chdb_search_channel
 * until it returns zero, then calls chdb_search_finish.
 */
extern void
chdb_search_select(
    chdbSearchConn* conn,
    Oid indexoid,
    uint64 generation,
    const char* sql
);

/*
 * Starts `sql`, an `INSERT INTO idx_<indexoid>.t_<generation> (cols)` with no FORMAT
 * clause, which the worker supplies as Native; the caller then sends blocks
 * through chdb_search_channel and ends the stream with chdb_search_finish,
 * which waits for the worker's acknowledgement and raises on failure.
 */
extern void
chdb_search_insert(
    chdbSearchConn* conn,
    Oid indexoid,
    uint64 generation,
    const char* sql
);

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

/* The worker's engine process: its pid, zero if none runs. */
extern int
chdb_search_engine_pid(chdbSearchConn* conn);

/* Sends a signal to the engine, which raises if none runs. Returns its pid. */
extern int
chdb_search_engine_kill(chdbSearchConn* conn, int signo);

/*
 * The channel of the connection, for the Native streaming code of native.h.
 * Use after chdb_search_select or chdb_search_insert; finish the stream with
 * chdb_search_finish.
 */
extern chdbChannel*
chdb_search_channel(chdbSearchConn* conn);

#endif /* CHDB_SEARCH_CLIENT_H */
