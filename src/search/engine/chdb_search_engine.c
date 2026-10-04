/*
 * The chDB engine behind a chdb_search worker.
 *
 * This is the only process of the search module that links libchdb, which
 * aborts or segfaults when an allocation fails. A signal death of the worker
 * itself would make the postmaster restart every backend, so the worker forks
 * this program and supervises it: a crash here costs the request in flight and
 * the worker starts another on the next one. See dev/design/chdb_search.md.
 *
 * Usage: chdb_search_engine <fd> <page fd> <store path> [readonly]
 *
 * Opens the store once and serves the requests of ../protocol.h on the
 * socketpair end <fd> until the supervisor closes it; the store's blobs are
 * the supervisor's, asked for on <page fd> (../pagestore/protocol.h). With
 * CHDB_SEARCH_ENGINE_READONLY as its last argument the supervisor runs on a
 * server in recovery, and the engine writes nothing (readonly.c). Error
 * text goes to stderr, which is the Postgres log. The supervisor sets the
 * process up as helper.c sets up chdb_helper: SIGPIPE at its default, death
 * with its parent.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chdb.h"

#include "commands.h"
#include "pagestore.h"
#include "readonly.h"
#include "session.h"

/* Serves one request. False means the framing is gone or the supervisor is. */
static bool
serve_request(int fd) {
    chdbSearchRequest req = { 0 };
    char* frame;
    bool keep = false;

    if (io_recv_request(fd, &req, &frame) == 1) {
        switch (req.ctx.cmd) {
        case CHDB_CMD_EXEC:
            keep = command_exec(fd, &req);
            break;
        case CHDB_CMD_SELECT:
            keep = command_select(fd, &req);
            break;
        case CHDB_CMD_INSERT:
            keep = command_insert(fd, &req);
            break;
        case CHDB_CMD_DROP:
            keep = command_drop(fd, &req);
            break;
        default:
            /* Unknown commands carry unknown data, so the framing is gone. */
            io_send_status(fd, CHDB_STATUS_ERROR, "unknown command");
            break;
        }
    }
    free(frame);

    return keep;
}

int
main(int argc, char** argv) {
    bool readonly = argc == 5 && strcmp(argv[4], CHDB_SEARCH_ENGINE_READONLY) == 0;

    if (argc != 4 && !readonly) {
        fprintf(
            stderr,
            "usage: chdb_search_engine <fd> <page fd> <store path> [" CHDB_SEARCH_ENGINE_READONLY
            "]\n"
        );
        return 2;
    }

    /*
     * chDB's handlers would run on whichever thread takes a fatal signal and
     * hide the signal from the supervisor, which reports it by name.
     */
    chdb_set_signal_handlers_enabled(0);
    readonly_set(readonly);
    if (!pagestore_start(atoi(argv[2])) || !pagestore_bootstrap()) {
        return 1;
    }
    session_open(argv[3]);
    if (readonly && !readonly_session()) {
        return 1;
    }

    int fd = atoi(argv[1]);
    while (serve_request(fd)) {}

    /* The host's exit hook of chdb.h: past it nothing asks for a blob. */
    session_close();
    chdb_shutdown();

    return 0;
}
