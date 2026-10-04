/*
 * Starting the chdb_helper process and trading Native blocks with it.
 *
 * libchdb often crashes when an allocation fails. Under postmaster Postgres
 * would kill every backend and run crash recovery. Instead run chDB in an
 * isolated process postmaster never registered.
 */

#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/palloc.h"

#include "channel.h"
#include "helper.h"
#include "setup.h"

/* The program that links libchdb, installed beside the extension library. */
#define CHDB_HELPER_PROGRAM "chdb_helper"

/* The channel comes first, so a chdbChannel* handed out is also the chdbHelper. */
typedef struct chdbHelper {
    chdbChannel ch;
    pid_t pid;
    int setup;     /* setup pipe write end, -1 once written */
    int data_peer; /* the helper's ends, held only until the fork */
    int err_peer;
    int setup_peer;
    const char* query;
    bool reaped;
    int status;     /* wait status, negative when waitpid gave none */
    int wait_errno; /* why waitpid gave none */
} chdbHelper;

/* Collects the helper's exit status. */
static int
reap(chdbHelper* h) {
    if (h->reaped) {
        return h->status;
    }

    while (waitpid(h->pid, &h->status, 0) < 0) {
        if (errno != EINTR) {
            h->wait_errno = errno;
            h->status     = -1;
            break;
        }
    }
    h->reaped = true;

    return h->status;
}

/* Waits for the helper's error output to end, then reaps it. */
static int
drain_and_reap(chdbHelper* h) {
    chdb_channel_drain_err(&h->ch);

    return reap(h);
}

/* The channel's cleanup has closed our ends; this kills the process behind them. */
static void
stop_helper(chdbChannel* ch) {
    chdbHelper* h = (chdbHelper*)ch;

    chdb_channel_close_fd(&h->setup);
    chdb_channel_close_fd(&h->data_peer);
    chdb_channel_close_fd(&h->err_peer);
    chdb_channel_close_fd(&h->setup_peer);
    if (h->pid > 0 && !h->reaped) {
        kill(h->pid, SIGKILL);
        reap(h);
    }
}

/* How the helper ended, for a report it left no message of its own for. */
static const char*
helper_end(chdbHelper* h) {
    if (h->status < 0) {
        return psprintf(
            "chDB (pid %d) could not be waited for: %s.",
            (int)h->pid,
            strerror(h->wait_errno)
        );
    }
    if (WIFEXITED(h->status)) {
        if (WEXITSTATUS(h->status) == CHDB_HELPER_LOST_BACKEND) {
            return "chDB lost the channel to the copy.";
        }

        return psprintf("chDB exited with status %d.", WEXITSTATUS(h->status));
    }

    return "chDB ended abnormally.";
}

/* Raises with whatever the helper managed to say before it went. */
static void
report_helper(chdbHelper* h, const char* what) {
    drain_and_reap(h);
    const char* detail = chdb_channel_error(&h->ch);

    if (h->status >= 0 && WIFSIGNALED(h->status)) {
        ereport(
            ERROR,
            errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
            errmsg("chdb: chDB was terminated by signal %d", WTERMSIG(h->status)),
            errdetail("%s", detail ? detail : "chDB produced no message."),
            errcontext("query: %s", h->query)
        );
    }

    ereport(
        ERROR,
        errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
        errmsg("chdb: %s", what),
        errdetail("%s", detail ? detail : helper_end(h)),
        errcontext("query: %s", h->query)
    );
}

/* Moves a descriptor clear of the standard ones the helper is about to take. */
static int
reserve_fd(int fd) {
    if (fd > CHDB_SETUP_FD) {
        return fd;
    }
    int high = fcntl(fd, F_DUPFD, 10);
    close(fd);

    return high;
}

/* dup2, except that a descriptor already in place only needs to stay open. */
static bool
place_fd(int fd, int target) {
    return fd == target ? fcntl(fd, F_SETFD, 0) == 0 : dup2(fd, target) == target;
}

/*
 * Everything between the fork and the exec runs in a process that still holds
 * the backend's Postgres state, so it may only _exit.
 */
static void
exec_helper(chdbHelper* h, const char* program, chdbHelperContext* ctx) {
    char* const argv[] = { (char*)program, NULL };
    int null           = open("/dev/null", O_RDWR);

    h->data_peer  = reserve_fd(h->data_peer);
    h->err_peer   = reserve_fd(h->err_peer);
    h->setup_peer = reserve_fd(h->setup_peer);
    null          = reserve_fd(null);

    /* The channel the query does not use must not reach the backend's own. */
    if (!place_fd(ctx->cmd == CHDB_CMD_INSERT ? h->data_peer : null, STDIN_FILENO) ||
        !place_fd(ctx->cmd == CHDB_CMD_INSERT ? null : h->data_peer, STDOUT_FILENO) ||
        !place_fd(h->err_peer, STDERR_FILENO) ||
        !place_fd(h->setup_peer, CHDB_SETUP_FD)) {
        _exit(126);
    }

    /* Postgres ignores SIGPIPE; ClickHouse wants the default disposition. */
    signal(SIGPIPE, SIG_DFL);
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif

    execv(argv[0], argv);
    _exit(127);
}

/* Hands the helper its setup payload, then closes the channel it arrived on. */
static void
write_setup(chdbHelper* h, const char* setup, size_t len) {
    if (!chdb_channel_try_write(&h->ch, h->setup, setup, len)) {
        /*
         * A closed read end means the helper is already going, and it accounts
         * for that better than errno does. Anything else leaves it waiting on
         * a payload that will not arrive, so report without waiting for it.
         */
        if (errno == EPIPE) {
            report_helper(h, "could not start chDB");
        }
        ereport(
            ERROR,
            errcode_for_socket_access(),
            errmsg("chdb: could not send the query to chDB: %m")
        );
    }
    chdb_channel_close_fd(&h->setup);
}

/* Both ends land in the caller before anything can raise, so cleanup owns them. */
static void
open_channel(int* first, int* second, bool duplex) {
    int fd[2];

    if ((duplex ? socketpair(AF_UNIX, SOCK_STREAM, 0, fd) : pipe(fd)) < 0) {
        ereport(
            ERROR,
            errcode_for_socket_access(),
            errmsg("chdb: could not open a channel to chDB: %m")
        );
    }
    *first  = fd[0];
    *second = fd[1];
}

/* Raises for a channel that broke, with whatever the helper said. */
static void
helper_failed(chdbChannel* ch, const char* what, int errnum pg_attribute_unused()) {
    report_helper((chdbHelper*)ch, what);
}

/* Data ended: the helper must have exited cleanly. */
static void
helper_ended(chdbChannel* ch) {
    chdbHelper* h = (chdbHelper*)ch;

    if (drain_and_reap(h) != 0) {
        report_helper(h, "error executing chDB query");
    }
}

chdbChannel*
chdb_helper_start(
    chdbHelperContext* ctx,
    const char* query,
    char* const* names,
    char* const* values,
    size_t nparams
) {
    chdbHelper* h = palloc0(sizeof(*h));

    chdb_channel_init(&h->ch, -1, -1);
    h->pid          = -1;
    h->setup        = -1;
    h->data_peer    = -1;
    h->err_peer     = -1;
    h->setup_peer   = -1;
    h->query        = query;
    h->ch.recv_what = "error fetching chDB query result";
    h->ch.send_what = "error appending to chDB query";
    h->ch.fail      = helper_failed;
    h->ch.ended     = helper_ended;
    h->ch.stop      = stop_helper;

    /* Registered first, so every descriptor below has an owner already. */
    chdb_channel_own(&h->ch);

    char pkglib[MAXPGPATH];
    get_pkglib_path(my_exec_path, pkglib);
    char program[MAXPGPATH];
    snprintf(program, sizeof(program), "%s/%s", pkglib, CHDB_HELPER_PROGRAM);
    if (access(program, X_OK) != 0) {
        ereport(
            ERROR,
            errcode_for_file_access(),
            errmsg("chdb: could not execute \"%s\": %m", program),
            errhint("The chdb extension installs chdb_helper beside its libraries.")
        );
    }

    StringInfoData setup;
    initStringInfo(&setup);
    chdb_helper_build_setup(&setup, ctx, query, names, values, nparams);

    open_channel(&h->ch.data, &h->data_peer, true);
    open_channel(&h->ch.err, &h->err_peer, false);
    open_channel(&h->setup_peer, &h->setup, false);

    /* Only the ends the helper is given may survive its exec. */
    chdb_channel_prepare_fd(h->ch.data);
    chdb_channel_prepare_fd(h->ch.err);
    chdb_channel_prepare_fd(h->setup);

    /* Postgres buffers would otherwise be flushed twice, once by each side. */
    fflush(NULL);
    h->pid = fork();
    if (h->pid < 0) {
        ereport(ERROR, errcode_for_file_access(), errmsg("chdb: could not fork: %m"));
    }
    if (h->pid == 0) {
        exec_helper(h, program, ctx);
    }
    elog(DEBUG1, "chdb: chdb_helper pid %d", (int)h->pid);

    chdb_channel_close_fd(&h->data_peer);
    chdb_channel_close_fd(&h->err_peer);
    chdb_channel_close_fd(&h->setup_peer);
    write_setup(h, setup.data, setup.len);
    pfree(setup.data);

    return &h->ch;
}

void
chdb_helper_finish(chdbChannel* ch) {
    chdbHelper* h = (chdbHelper*)ch;

    /* End of stream for the helper, which then finishes its insert. */
    chdb_channel_end_write(ch);

    if (drain_and_reap(h) != 0) {
        report_helper(h, "error finishing chDB query");
    }
}
