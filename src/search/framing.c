/*
 * Reading requests and writing data and status frames on a client socket.
 * Every wait is on the latch, so a shutdown request ends it.
 */

#include "postgres.h"

#include <errno.h>
#include <sys/socket.h>

#include "miscadmin.h"
#include "postmaster/interrupt.h"
#include "storage/latch.h"
#include "utils/wait_event.h"

#include "framing.h"

static void
wait_socket(int fd, uint32 event) {
    WaitLatchOrSocket(
        MyLatch, event | WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, fd, -1, PG_WAIT_EXTENSION
    );
    ResetLatch(MyLatch);
}

/*
 * Reads exactly `len` bytes. Returns 1 on success, 0 on a clean end of
 * stream before the first byte, and -1 for anything that leaves the framing
 * untrustworthy, including a shutdown request.
 */
int
frame_recv(int fd, void* buf, size_t len) {
    size_t got = 0;

    while (got < len) {
        CHECK_FOR_INTERRUPTS();
        if (ShutdownRequestPending) {
            return -1;
        }

        ssize_t n = recv(fd, (char*)buf + got, len - got, 0);
        if (n > 0) {
            got += (size_t)n;
        } else if (n == 0) {
            return got == 0 ? 0 : -1;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            wait_socket(fd, WL_SOCKET_READABLE);
        } else if (errno != EINTR) {
            return -1;
        }
    }

    return 1;
}

/* MSG_NOSIGNAL: a client that went away is an error here, not a SIGPIPE. */
bool
frame_send(int fd, const void* buf, size_t len) {
    size_t put = 0;

    while (put < len) {
        CHECK_FOR_INTERRUPTS();
        if (ShutdownRequestPending) {
            return false;
        }

        ssize_t n = send(fd, (const char*)buf + put, len - put, MSG_NOSIGNAL);
        if (n > 0) {
            put += (size_t)n;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            wait_socket(fd, WL_SOCKET_WRITEABLE);
        } else if (errno != EINTR) {
            return false;
        }
    }

    return true;
}

/* Sends one data chunk, splitting it so no chunk exceeds what clients accept. */
bool
frame_send_chunks(int fd, const char* buf, size_t len) {
    while (len) {
        uint32_t n = (uint32_t)Min(len, CHDB_SEARCH_CHUNK_MAX);

        if (!frame_send(fd, &n, sizeof(n)) || !frame_send(fd, buf, n)) {
            return false;
        }
        buf += n;
        len -= n;
    }

    return true;
}

bool
frame_send_end(int fd) {
    uint32_t zero = 0;

    return frame_send(fd, &zero, sizeof(zero));
}

bool
frame_send_status(int fd, const char* err) {
    uint8_t status = err ? 1 : 0;
    uint32_t len   = err ? (uint32_t)strlen(err) : 0;

    return frame_send(fd, &status, sizeof(status)) &&
           frame_send(fd, &len, sizeof(len)) && (len == 0 || frame_send(fd, err, len));
}

/* Reads a length-prefixed string into request memory, NUL terminated. */
static int
recv_string(int fd, char** out, size_t* out_len) {
    uint32_t len;

    if (frame_recv(fd, &len, sizeof(len)) != 1 || len > CHDB_SETUP_MAX) {
        return -1;
    }
    *out = palloc(len + 1);
    if (len && frame_recv(fd, *out, len) != 1) {
        return -1;
    }
    (*out)[len] = '\0';
    if (out_len) {
        *out_len = len;
    }

    return 1;
}

/* 1 for a request, 0 when the client hung up between requests, -1 on a bad one. */
int
frame_recv_request(int fd, request* req, bool* has_params) {
    uint8_t head[1 + 4 + 2 + 2 + 2];
    int rc = frame_recv(fd, head, sizeof(head));

    if (rc != 1) {
        return rc;
    }
    req->cmd = head[0];
    memcpy(&req->index, head + 1, 4);
    memcpy(&req->max_memory, head + 5, 2);
    memcpy(&req->max_threads, head + 7, 2);
    memcpy(&req->max_parsers, head + 9, 2);

    if (recv_string(fd, &req->query, &req->query_len) != 1) {
        return -1;
    }

    uint16_t nparams;
    if (frame_recv(fd, &nparams, sizeof(nparams)) != 1) {
        return -1;
    }
    for (uint16_t i = 0; i < nparams; i++) {
        char* skip;

        if (recv_string(fd, &skip, NULL) != 1 || recv_string(fd, &skip, NULL) != 1) {
            return -1;
        }
    }
    *has_params = nparams > 0;

    return 1;
}
