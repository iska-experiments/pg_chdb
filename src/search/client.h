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
 * Every request names the index it works on. The worker maps an index OID to
 * the chDB database `idx_<indexoid>` and keeps one table `t` in it.
 */

#include "postgres.h"

#include "nodes/pg_list.h"
#include "utils/relcache.h"

/* Commands, continuing the numbering of src/setup.h. */
#define CHDB_CMD_EXEC 'E'   /* run a statement, no result rows */
#define CHDB_CMD_DROP 'X'   /* drop the index's chDB database */

typedef struct chdbSearchConn chdbSearchConn;

/* Opens a connection to the worker, starting it if needed. Never NULL. */
extern chdbSearchConn*
chdb_search_connect(void);

/* Closes the connection. Safe on an already-closed connection. */
extern void
chdb_search_close(chdbSearchConn* conn);

/*
 * Runs `sql` in the index's database. `sql` may reference the table as `t`.
 * Raises on failure with the worker's error text.
 */
extern void
chdb_search_exec(chdbSearchConn* conn, Oid indexoid, const char* sql);

/*
 * Runs `sql` and returns a stream of Native blocks. The caller reads it with
 * chdb_search_recv until it returns zero, then calls chdb_search_finish.
 */
extern void
chdb_search_select(chdbSearchConn* conn, Oid indexoid, const char* sql);

extern size_t
chdb_search_recv(chdbSearchConn* conn, void* buf, size_t len);

/*
 * Starts `INSERT INTO t (cols) FORMAT Native`; the caller then sends blocks
 * with chdb_search_send and ends the stream with chdb_search_finish, which
 * waits for the worker's acknowledgement and raises on failure.
 */
extern void
chdb_search_insert(chdbSearchConn* conn, Oid indexoid, const char* sql);

extern void
chdb_search_send(chdbSearchConn* conn, const void* buf, size_t len);

extern void
chdb_search_finish(chdbSearchConn* conn);

/* Drops the index's chDB database. Idempotent. */
extern void
chdb_search_drop(chdbSearchConn* conn, Oid indexoid);

#endif /* CHDB_SEARCH_CLIENT_H */
