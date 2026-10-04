#ifndef CHDB_SEARCH_SERVE_H
#define CHDB_SEARCH_SERVE_H

/* The worker's listening socket and the loop serving clients on it. */

#include "postgres.h"

/* Binds the socket of database `dboid`. */
extern void
chdb_search_listen(Oid dboid);

/* Serves clients until a shutdown request. */
extern void
chdb_search_serve(void);

/* Removes the socket file, if bound. */
extern void
chdb_search_unlisten(void);

#endif /* CHDB_SEARCH_SERVE_H */
