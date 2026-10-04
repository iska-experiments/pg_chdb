#ifndef CHDB_SEARCH_FRAMING_H
#define CHDB_SEARCH_FRAMING_H

/*
 * The worker's end of the framing of protocol.h: reading requests and writing
 * chunks and status frames on a client socket, waiting on the latch.
 */

#include "postgres.h"

#include "protocol.h"

typedef struct request {
    chdbCmdType cmd;
    Oid index;
    uint16_t max_memory;
    uint16_t max_threads;
    uint16_t max_parsers;
    char* query;
    size_t query_len;
} request;

/*
 * Reads exactly `len` bytes. Returns 1 on success, 0 on a clean end of
 * stream before the first byte, and -1 for anything that leaves the framing
 * untrustworthy, including a shutdown request.
 */
extern int
frame_recv(int fd, void* buf, size_t len);

extern bool
frame_send(int fd, const void* buf, size_t len);

/* Sends one data chunk, splitting it so no chunk exceeds what clients accept. */
extern bool
frame_send_chunks(int fd, const char* buf, size_t len);

extern bool
frame_send_end(int fd);

extern bool
frame_send_status(int fd, const char* err);

/* 1 for a request, 0 when the client hung up between requests, -1 on a bad one. */
extern int
frame_recv_request(int fd, request* req, bool* has_params);

#endif /* CHDB_SEARCH_FRAMING_H */
