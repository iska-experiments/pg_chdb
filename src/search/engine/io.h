#ifndef CHDB_SEARCH_ENGINE_IO_H
#define CHDB_SEARCH_ENGINE_IO_H

/*
 * The engine's end of the framing of ../protocol.h on the socketpair to its
 * supervisor. Blocking, with no Postgres in the process.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../protocol.h"

typedef struct request {
    chdbCmdType cmd;
    uint32_t index;
    uint64_t generation; /* of the table the query works on, zero for none */
    uint16_t max_memory;
    uint16_t max_threads;
    uint16_t max_parsers;
    char* query;
    size_t query_len;
} request;

/* 1 on success, 0 on a clean end of stream before the first byte, else -1. */
extern int
io_recv(int fd, void* buf, size_t len);

extern bool
io_send(int fd, const void* buf, size_t len);

/* Sends data as chunks no larger than the protocol allows. */
extern bool
io_send_chunks(int fd, const char* buf, size_t len);

extern bool
io_send_end(int fd);

/* The status frame: `status` is CHDB_STATUS_OK when `err` is NULL. */
extern bool
io_send_status(int fd, uint8_t status, const char* err);

/* 1 for a request, 0 when the supervisor closed between requests, -1 on a bad one. */
extern int
io_recv_request(int fd, request* req, bool* has_params);

extern void
io_free_request(request* req);

#endif /* CHDB_SEARCH_ENGINE_IO_H */
