#ifndef CHDB_SEARCH_ENGINE_PAGECALL_H
#define CHDB_SEARCH_ENGINE_PAGECALL_H

/*
 * One page request of ../pagestore/protocol.h to the supervisor, from any
 * thread of the engine: sent whole, answered by the reader thread that
 * matches replies to their requests by id.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A reply. With `fixed` set, `body` and `cap` name the caller's buffer, which
 * takes a successful reply's body; otherwise the body is malloc'd for the
 * caller to free, NUL-terminated past its length. On failure the body holds
 * nothing and the error text is pagecall_last_error's.
 */
typedef struct pageReply {
    uint32_t status;
    char* body;
    size_t len;
    size_t cap;
    bool fixed;
} pageReply;

/* Starts the reader thread on the page socket. False, with stderr told, if it cannot.
 */
extern bool
pagecall_start(int fd);

/*
 * Sends `op` with a body of `head` followed by `data`, which lets an append
 * send the engine's buffer without copying it, and waits for the reply.
 * Returns its status; CHDB_PAGE_OK means `reply` holds the body.
 */
extern uint32_t
pagecall(
    uint32_t op,
    const void* head,
    size_t headlen,
    const void* data,
    size_t datalen,
    pageReply* reply
);

/* The last failure on the calling thread, as libchdb's last_error wants it. */
extern const char*
pagecall_last_error(void);

extern void
pagecall_set_error(const char* text, size_t len);

/* Sets the error and returns 1, the callbacks' failure. */
extern int
pagecall_fail(const char* msg);

/* The body of a request. Keys and names are short; one that would not fit fails. */
typedef struct pageBody {
    char data[1024];
    size_t len;
    bool overflow;
} pageBody;

extern void
pagebody_put(pageBody* b, const void* p, size_t n);
extern void
pagebody_str(pageBody* b, const char* s);
extern void
pagebody_u64(pageBody* b, uint64_t v);

/* pagecall with a body, as the callbacks want it: nonzero with the error set on
 * failure. */
extern int
pagecall_body(
    uint32_t op,
    const pageBody* b,
    const void* data,
    size_t datalen,
    pageReply* r
);

#endif /* CHDB_SEARCH_ENGINE_PAGECALL_H */
