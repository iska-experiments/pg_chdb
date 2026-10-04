#ifndef CHDB_SEARCH_REQUEST_H
#define CHDB_SEARCH_REQUEST_H

/* Reading one request of protocol.h from a client socket. */

#include "postgres.h"

#include "protocol.h"

typedef struct request {
    Oid index;
    chdbHelperContext ctx; /* command and limits */
    chdbSetupStr query;    /* borrowed from the frame */
    uint16_t nparams;
} request;

/*
 * 1 for a request, 0 when the client hung up between requests, -1 for one the
 * framing cannot be trusted after. The frame `req` borrows from is palloc'd
 * in the current memory context.
 */
extern int
request_recv(int fd, request* req);

#endif /* CHDB_SEARCH_REQUEST_H */
