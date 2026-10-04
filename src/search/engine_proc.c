/*
 * Forking, feeding and reaping the chdb_search_engine child. See
 * engine_proc.h and src/search/engine/chdb_search_engine.c.
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

#include "miscadmin.h"

#include "../channel.h"
#include "engine_proc.h"

#define CHDB_SEARCH_ENGINE_PROGRAM "chdb_search_engine"

/* The descriptor the engine is handed its end of the socketpair as. */
#define CHDB_SEARCH_ENGINE_FD 3

/* Milliseconds an engine gets to close its store when told to stop. */
#define CHDB_SEARCH_STOP_MS 5000

static struct {
    pid_t pid; /* 0 when none runs */
    chdbChannel ch;
} engine;

/* Raised by the channel when the engine's end breaks; engine_send and recv catch it. */
static void
engine_lost(chdbChannel* ch pg_attribute_unused(), const char* what, int errnum) {
    ereport(
        ERROR,
        errmsg("chdb_search: %s", what),
        errdetail("%s", errnum ? strerror(errnum) : "end of stream")
    );
}

bool
engine_send(const void* buf, size_t len) {
    bool ok = engine.pid > 0;

    if (ok) {
        PG_TRY();
        { chdb_channel_send_exact(&engine.ch, buf, len); }
        PG_CATCH();
        {
            FlushErrorState();
            ok = false;
        }
        PG_END_TRY();
    }

    return ok;
}

bool
engine_recv(void* buf, size_t len) {
    bool ok = engine.pid > 0;

    if (ok) {
        PG_TRY();
        { chdb_channel_recv_exact(&engine.ch, buf, len); }
        PG_CATCH();
        {
            FlushErrorState();
            ok = false;
        }
        PG_END_TRY();
    }

    return ok;
}

pid_t
engine_pid(void) {
    return engine.pid;
}

static int
wait_status(pid_t pid, int flags, bool* gone) {
    int status = 0;
    pid_t rc;

    while ((rc = waitpid(pid, &status, flags)) < 0 && errno == EINTR) {}
    *gone = rc == pid;

    return status;
}

char*
engine_death(void) {
    pid_t pid = engine.pid;
    bool gone;

    if (pid <= 0) {
        return pstrdup("chDB engine is not running");
    }
    /* A closed socket from a live process is a protocol failure; end it. */
    kill(pid, SIGKILL);
    int status = wait_status(pid, 0, &gone);

    chdb_channel_close(&engine.ch);
    engine.pid = 0;

    char* msg;

    if (!gone) {
        msg = psprintf("chDB engine (pid %d) could not be waited for", (int)pid);
    } else if (WIFSIGNALED(status)) {
        msg = psprintf(
            "chDB engine (pid %d) was terminated by signal %d: %s",
            (int)pid,
            WTERMSIG(status),
            pg_strsignal(WTERMSIG(status))
        );
    } else if (WIFEXITED(status)) {
        msg = psprintf(
            "chDB engine (pid %d) exited with status %d", (int)pid, WEXITSTATUS(status)
        );
    } else {
        msg = psprintf("chDB engine (pid %d) ended abnormally", (int)pid);
    }
    ereport(LOG, errmsg("chdb_search: %s", msg));

    return msg;
}

void
engine_stop(void) {
    pid_t pid = engine.pid;

    if (pid <= 0) {
        return;
    }
    /* The engine reads the close as the end of its requests and exits cleanly. */
    chdb_channel_close(&engine.ch);
    for (int waited = 0; waited < CHDB_SEARCH_STOP_MS; waited += 10) {
        bool gone;

        wait_status(pid, WNOHANG, &gone);
        if (gone) {
            engine.pid = 0;
            return;
        }
        pg_usleep(10 * 1000);
    }
    kill(pid, SIGKILL);
    wait_status(pid, 0, &(bool){ false });
    engine.pid = 0;
}

/*
 * Between the fork and the exec the child still holds the worker's Postgres
 * state, so it may only _exit. Set up as helper.c sets up chdb_helper.
 */
static void
exec_engine(int fd, char* const argv[]) {
    sigset_t none;

    if (!chdb_channel_place_fd(fd, CHDB_SEARCH_ENGINE_FD)) {
        _exit(126);
    }
    /* Nothing else of the worker's reaches the engine. */
#ifdef __linux__
    close_range(CHDB_SEARCH_ENGINE_FD + 1, ~0U, 0);
#else
    for (int i = CHDB_SEARCH_ENGINE_FD + 1; i < 1024; i++) {
        close(i);
    }
#endif

    /* Postgres ignores SIGPIPE; ClickHouse wants the default disposition. */
    signal(SIGPIPE, SIG_DFL);
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
#ifdef __linux__
    /* Die with the worker, whose store lock a successor must be able to take. */
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    if (getppid() != MyProcPid) {
        _exit(1); /* the worker went before the prctl took hold */
    }

    execv(argv[0], argv);
    _exit(127);
}

char*
engine_ensure(Oid dboid) {
    if (engine.pid > 0) {
        return NULL;
    }

    char pkglib[MAXPGPATH];
    get_pkglib_path(my_exec_path, pkglib);
    char* program = psprintf("%s/%s", pkglib, CHDB_SEARCH_ENGINE_PROGRAM);

    if (access(program, X_OK) != 0) {
        return psprintf("could not execute \"%s\": %m", program);
    }

    int fd[2];

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd) < 0) {
        return psprintf("could not open a channel to the chDB engine: %m");
    }

    char* const argv[] = {
        program,
        CppAsString2(CHDB_SEARCH_ENGINE_FD),
        psprintf("%s/pg_chdb/%u", DataDir, dboid),
        NULL,
    };

    /* Postgres buffers would otherwise be flushed twice, once by each side. */
    fflush(NULL);
    pid_t pid = fork();

    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        return psprintf("could not fork the chDB engine: %m");
    }
    if (pid == 0) {
        exec_engine(fd[1], argv);
    }
    close(fd[1]);

    chdb_channel_init(&engine.ch, fd[0], -1);
    engine.ch.recv_what = "error receiving from the chDB engine";
    engine.ch.send_what = "error sending to the chDB engine";
    engine.ch.fail      = engine_lost;
    engine.pid          = pid;
    chdb_channel_prepare_fd(fd[0]);
    elog(DEBUG1, "chdb_search: chdb_search_engine pid %d", (int)pid);

    return NULL;
}
