#ifndef CHDB_SEARCH_REQUEST_H
#define CHDB_SEARCH_REQUEST_H

/* Serving one request of a client connection. */

#include "postgres.h"

/*
 * Reads a request from `fd` and answers it, from the engine or, for the debug
 * commands, from the worker itself. False means close the connection.
 */
extern bool
chdb_search_serve_request(int fd, MemoryContext cxt);

#endif /* CHDB_SEARCH_REQUEST_H */
