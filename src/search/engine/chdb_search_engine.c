/*
 * The chDB engine behind a chdb_search worker.
 *
 * This is the only process of the search module that links libchdb, which
 * aborts or segfaults when an allocation fails. A signal death of the worker
 * itself would make the postmaster restart every backend, so the worker forks
 * this program and supervises it: a crash here costs the request in flight and
 * the worker starts another on the next one. See dev/design/chdb_search.md.
 *
 * Usage: chdb_search_engine <fd> <store path> <supervisor pid>
 *
 * Opens the store once and serves the requests of ../protocol.h on the
 * socketpair end <fd> until the supervisor closes it. Error text goes to
 * stderr, which is the Postgres log.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "chdb.h"

#include "commands.h"
#include "session.h"

/* Serves one request. False means the framing is gone or the supervisor is. */
static bool
serve_request(int fd) {
    request req     = { 0 };
    bool has_params = false;
    bool keep       = false;

    if (io_recv_request(fd, &req, &has_params) == 1) {
        if (has_params) {
            keep = io_send_status(fd, "query parameters are not supported");
        } else {
            switch (req.cmd) {
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
                io_send_status(fd, "unknown command");
                break;
            }
        }
    }
    io_free_request(&req);

    return keep;
}

int
main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(
            stderr, "usage: chdb_search_engine <fd> <store path> <supervisor pid>\n"
        );
        return 2;
    }

    int fd           = atoi(argv[1]);
    pid_t supervisor = (pid_t)atoi(argv[3]);

    /* Postgres ignores SIGPIPE; ClickHouse wants the default disposition. */
    signal(SIGPIPE, SIG_DFL);
#ifdef __linux__
    /* Die with the supervisor, whose store lock a successor must be able to take. */
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    if (getppid() != supervisor) {
        return 1; /* the supervisor went before the prctl took hold */
    }

    /*
     * chDB's handlers would run on whichever thread takes a fatal signal and
     * hide the signal from the supervisor, which reports it by name.
     */
    chdb_set_signal_handlers_enabled(0);
    session_open(argv[2]);

    while (serve_request(fd)) {}

    session_close();

    return 0;
}
