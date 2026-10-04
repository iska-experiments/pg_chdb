/*
 * Reading requests and writing data and status frames on the supervisor's
 * socket. See ../protocol.h.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "io.h"

int
io_recv(int fd, void* buf, size_t len) {
    size_t got = 0;

    while (got < len) {
        ssize_t n = recv(fd, (char*)buf + got, len - got, 0);

        if (n > 0) {
            got += (size_t)n;
        } else if (n == 0) {
            return got == 0 ? 0 : -1;
        } else if (errno != EINTR) {
            return -1;
        }
    }

    return 1;
}

/* MSG_NOSIGNAL: a supervisor that went away is an error here, not a SIGPIPE. */
bool
io_send(int fd, const void* buf, size_t len) {
    size_t put = 0;

    while (put < len) {
        ssize_t n = send(fd, (const char*)buf + put, len - put, MSG_NOSIGNAL);

        if (n > 0) {
            put += (size_t)n;
        } else if (errno != EINTR) {
            return false;
        }
    }

    return true;
}

bool
io_send_chunks(int fd, const char* buf, size_t len) {
    while (len) {
        uint32_t n =
            len < CHDB_SEARCH_CHUNK_MAX ? (uint32_t)len : CHDB_SEARCH_CHUNK_MAX;

        if (!io_send(fd, &n, sizeof(n)) || !io_send(fd, buf, n)) {
            return false;
        }
        buf += n;
        len -= n;
    }

    return true;
}

bool
io_send_end(int fd) {
    uint32_t zero = 0;

    return io_send(fd, &zero, sizeof(zero));
}

bool
io_send_status(int fd, const char* err) {
    uint8_t status = err ? 1 : 0;
    uint32_t len   = err ? (uint32_t)strlen(err) : 0;

    return io_send(fd, &status, sizeof(status)) && io_send(fd, &len, sizeof(len)) &&
           (len == 0 || io_send(fd, err, len));
}

/* Reads a length-prefixed string, NUL terminated. */
static int
recv_string(int fd, char** out, size_t* out_len) {
    uint32_t len;

    if (io_recv(fd, &len, sizeof(len)) != 1 || len > CHDB_SETUP_MAX) {
        return -1;
    }
    *out = malloc((size_t)len + 1);
    if (!*out) {
        return -1;
    }
    if (len && io_recv(fd, *out, len) != 1) {
        return -1;
    }
    (*out)[len] = '\0';
    if (out_len) {
        *out_len = len;
    }

    return 1;
}

int
io_recv_request(int fd, request* req, bool* has_params) {
    uint8_t head[1 + 4 + 2 + 2 + 2];
    int rc = io_recv(fd, head, sizeof(head));

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
    if (io_recv(fd, &nparams, sizeof(nparams)) != 1) {
        return -1;
    }
    for (uint16_t i = 0; i < nparams; i++) {
        char* skip;

        if (recv_string(fd, &skip, NULL) != 1) {
            return -1;
        }
        free(skip);
        if (recv_string(fd, &skip, NULL) != 1) {
            return -1;
        }
        free(skip);
    }
    *has_params = nparams > 0;

    return 1;
}

void
io_free_request(request* req) {
    free(req->query);
    req->query = NULL;
}
