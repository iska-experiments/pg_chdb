/*
 * Reading one request of protocol.h: a frame read whole, then decoded in
 * memory with the setup payload decoders of src/setup.h, so the worker never
 * walks the layout itself.
 */

#include "postgres.h"

#include "framing.h"
#include "request.h"

int
request_recv(int fd, request* req) {
    uint32_t len;
    int rc = frame_recv(fd, &len, sizeof(len));

    if (rc != 1) {
        return rc;
    }
    if (len > CHDB_SETUP_MAX) {
        return -1;
    }

    char* frame = palloc(len);
    if (frame_recv(fd, frame, len) != 1) {
        return -1;
    }

    chdbSetupCursor cur = { .at = frame, .end = frame + len };
    if (!chdb_setup_take(&cur, &req->index, sizeof(req->index)) ||
        !chdb_setup_parse_head(&cur, &req->ctx, &req->query, &req->nparams)) {
        return -1;
    }

    return 1;
}
