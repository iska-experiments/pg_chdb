/*
 * Feeding, reaping and stopping the chdb_search_engine child that
 * engine_spawn.c forks, and answering its page requests on the second
 * socketpair it is handed, whenever the worker is not inside a request of
 * its own: from the event loop (serve.c), from inside any wait on the
 * request channel (channel.h's aside), and while waiting for it to stop. See
 * engine_proc.h and src/search/engine/chdb_search_engine.c.
 */

#include "postgres.h"

#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "miscadmin.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "utils/wait_event.h"

#include "../channel.h"
#include "engine_proc.h"
#include "pagestore/pagestore.h"

/* Milliseconds an engine gets to close its store when told to stop. */
#define CHDB_SEARCH_STOP_MS 5000

static struct {
    pid_t pid;        /* 0 when none runs */
    chdbChannel ch;   /* its requests and replies */
    chdbChannel page; /* its page requests */
    bool reaped;      /* waited for already, with this status */
    int status;
} engine;

/* Raised by a channel when the engine's end breaks; engine_send and recv catch it. */
static void
engine_lost(chdbChannel* ch pg_attribute_unused(), const char* what, int errnum) {
    ereport(
        ERROR,
        errmsg("chdb_search: %s", what),
        errdetail("%s", errnum ? strerror(errnum) : "end of stream")
    );
}

/*
 * The failure is logged and swallowed: the caller reports the engine's death,
 * which the log then explains, whether the channel broke or a page request
 * served inside its wait failed.
 */
static bool
engine_io(bool send, void* buf, size_t len) {
    MemoryContext old = CurrentMemoryContext;
    bool ok           = engine.pid > 0;

    if (ok) {
        PG_TRY();
        {
            if (send) {
                chdb_channel_send_exact(&engine.ch, buf, len);
            } else {
                chdb_channel_recv_exact(&engine.ch, buf, len);
            }
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(old);
            EmitErrorReport();
            FlushErrorState();
            ok = false;
        }
        PG_END_TRY();
    }

    return ok;
}

bool
engine_send(const void* buf, size_t len) {
    return engine_io(true, (void*)buf, len);
}

bool
engine_recv(void* buf, size_t len) {
    return engine_io(false, buf, len);
}

pid_t
engine_pid(void) {
    return engine.pid;
}

chdbChannel*
engine_channel(void) {
    return engine.pid > 0 ? &engine.ch : NULL;
}

static int
wait_status(pid_t pid, int flags, bool* gone) {
    int status = 0;
    pid_t rc;

    while ((rc = waitpid(pid, &status, flags)) < 0 && errno == EINTR) {}
    *gone = rc == pid;

    return status;
}

/* Closes what is left of a reaped engine. */
static void
forget_engine(void) {
    chdb_channel_close(&engine.ch);
    chdb_channel_close(&engine.page);
    chdb_pagestore_engine_gone();
    engine.pid    = 0;
    engine.reaped = false;
}

char*
engine_death(void) {
    pid_t pid = engine.pid;

    if (pid <= 0) {
        return pstrdup("chDB engine is not running");
    }
    /* A closed socket from a live process is a protocol failure; end it. */
    if (!engine.reaped) {
        kill(pid, SIGKILL);
        engine.status = wait_status(pid, 0, &engine.reaped);
    }

    bool gone  = engine.reaped;
    int status = engine.status;
    char* msg;

    forget_engine();
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

/* ---- the page channel ---- */

int
engine_page_fd(void) {
    return engine.pid > 0 ? engine.page.data : -1;
}

/*
 * The page channel broke. An engine that died is left for the next request
 * to report, as it was before the channel told; a live one garbled its
 * framing and cannot be trusted, so it is put down. Its pending writes go
 * either way.
 */
static void
page_broke(void) {
    chdb_channel_close(&engine.page);
    engine.ch.aside_fd = -1;
    for (int waited = 0; !engine.reaped && waited < 1000; waited += 10) {
        engine.status = wait_status(engine.pid, WNOHANG, &engine.reaped);
        if (!engine.reaped) {
            pg_usleep(10 * 1000);
        }
    }
    if (engine.reaped) {
        chdb_pagestore_engine_gone();
    } else {
        pfree(engine_death());
    }
}

bool
engine_serve_page(void) {
    MemoryContext old = CurrentMemoryContext;
    bool ok           = engine.pid > 0 && engine.page.data >= 0;

    if (ok) {
        PG_TRY();
        { chdb_pagestore_serve(&engine.page); }
        PG_CATCH();
        {
            MemoryContextSwitchTo(old);
            EmitErrorReport();
            FlushErrorState();
            ok = false;
        }
        PG_END_TRY();
        if (!ok) {
            page_broke();
        }
    }

    return ok;
}

/* channel.h's aside: a page request arrived while a request wait slept. */
static void
serve_aside(chdbChannel* ch pg_attribute_unused()) {
    chdb_pagestore_serve(&engine.page);
}

/*
 * Closing its store, the engine may still ask for pages, for the merges it
 * cancels, so they are served while it is waited for: by the worker's main
 * loop once its serving is over, not from an exit callback, inside which an
 * error cannot be caught. The channel would not wait once a shutdown is
 * pending, which is when the worker stops its engine.
 */
void
engine_stop(void) {
    pid_t pid     = engine.pid;
    bool shutdown = ShutdownRequestPending;
    char peek;

    if (pid <= 0) {
        return;
    }
    /* The engine reads the close as the end of its requests and exits cleanly. */
    chdb_channel_close(&engine.ch);
    ShutdownRequestPending = false;
    for (int waited = 0; engine.pid == pid && !engine.reaped && !proc_exit_inprogress &&
                         waited < CHDB_SEARCH_STOP_MS;
         waited += 10) {
        engine.status = wait_status(pid, WNOHANG, &engine.reaped);
        if (engine.reaped) {
            break;
        }
        if (engine.page.data < 0) {
            pg_usleep(10 * 1000);
        } else if (
            WaitLatchOrSocket(
                MyLatch,
                WL_SOCKET_READABLE | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                engine.page.data,
                10,
                PG_WAIT_EXTENSION
            ) &
            WL_SOCKET_READABLE
        ) {
            /* The end of the stream is the engine on its way out, not a request. */
            if (recv(engine.page.data, &peek, 1, MSG_PEEK | MSG_DONTWAIT) > 0) {
                engine_serve_page();
            } else {
                chdb_channel_close(&engine.page);
                engine.ch.aside_fd = -1;
            }
        }
    }
    ShutdownRequestPending = shutdown;
    if (engine.pid != pid) {
        return; /* put down meanwhile */
    }
    if (!engine.reaped) {
        kill(pid, SIGKILL);
        wait_status(pid, 0, &engine.reaped);
    }
    forget_engine();
}

/* ---- attaching the child engine_spawn.c started ---- */

static void
open_channel(chdbChannel* ch, int fd, const char* recv_what, const char* send_what) {
    chdb_channel_init(ch, fd, -1);
    ch->recv_what = recv_what;
    ch->send_what = send_what;
    ch->fail      = engine_lost;
    chdb_channel_prepare_fd(fd);
}

void
engine_attach(pid_t pid, int fd, int page_fd) {
    open_channel(
        &engine.ch,
        fd,
        "error receiving from the chDB engine",
        "error sending to the chDB engine"
    );
    open_channel(
        &engine.page,
        page_fd,
        "error receiving the chDB engine's page request",
        "error answering the chDB engine's page request"
    );
    engine.ch.aside_fd = page_fd;
    engine.ch.aside    = serve_aside;
    engine.pid         = pid;
    engine.reaped      = false;
    elog(DEBUG1, "chdb_search: chdb_search_engine pid %d", (int)pid);
}
