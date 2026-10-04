#ifndef CHDB_SEARCH_SERVE_H
#define CHDB_SEARCH_SERVE_H

/* The worker's listening socket and the loop serving clients on it. */

#include "postgres.h"

#include <sys/socket.h>
#include <sys/un.h>

/*
 * The address the worker of database `dboid` listens on and backends
 * connect to, and its length for bind() and connect(): a name in the
 * abstract namespace on Linux, else the file pg_chdb/pgsql_tmp/<dboid>.sock
 * in the data directory (serve.c).
 */
extern socklen_t
chdb_search_socket_addr(Oid dboid, struct sockaddr_un* addr);

/* Binds the socket of database `dboid`. */
extern void
chdb_search_listen(Oid dboid);

/* Serves clients until a shutdown request. */
extern void
chdb_search_serve(void);

/* Removes the socket file, if one is bound. */
extern void
chdb_search_unlisten(void);

#endif /* CHDB_SEARCH_SERVE_H */
