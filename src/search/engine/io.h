#ifndef CHDB_SEARCH_ENGINE_IO_H
#define CHDB_SEARCH_ENGINE_IO_H

/*
 * The engine's end of the framing of ../protocol.h on the socketpair to its
 * supervisor. Blocking, with no Postgres in the process.
 */

#include <stdbool.h>
#include <stddef.h>

#include "../protocol.h"

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

/* The status frame: success when `err` is NULL. */
extern bool
io_send_status(int fd, const char* err);

/*
 * Reads and decodes one request. 1 for a request, whose strings borrow from
 * `*frame`, malloc'd for the caller to free; 0 when the supervisor closed
 * between requests; -1 for one the framing cannot be trusted after.
 */
extern int
io_recv_request(int fd, chdbSearchRequest* req, char** frame);

#endif /* CHDB_SEARCH_ENGINE_IO_H */
