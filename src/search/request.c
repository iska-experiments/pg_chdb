/*
 * Reading one request of protocol.h: a frame read whole, then decoded in
 * memory by protocol.h, so the worker never walks the layout itself.
 */

#include "postgres.h"

#include "framing.h"
#include "request.h"

int
request_recv(int fd, chdbSearchRequest* req) {
    uint32_t len;
    int rc = frame_recv(fd, &len, sizeof(len));

    if (rc != 1) {
        return rc;
    }
    if (len > CHDB_SETUP_MAX) {
        return -1;
    }

    char* frame = palloc(len);

    if (frame_recv(fd, frame, len) != 1 ||
        !chdb_search_decode_request(frame, len, req)) {
        return -1;
    }

    return 1;
}
