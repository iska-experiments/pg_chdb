/*
 * Forking the chdb_search_engine child: the two socketpairs it is handed, the
 * program beside the library, and the exec preamble of spawn.c. The running
 * engine is then engine_proc.c's. See engine_proc.h.
 */

#include "postgres.h"

#include <sys/socket.h>
#include <unistd.h>

#include "miscadmin.h"

#include "../spawn.h"
#include "engine_proc.h"
#include "pagestore/protocol.h"
#include "protocol.h"

#define CHDB_SEARCH_ENGINE_PROGRAM "chdb_search_engine"

/* The descriptor the engine is handed its end of the request socketpair as. */
#define CHDB_SEARCH_ENGINE_FD 3

/*
 * Between the fork and the exec the child still holds the worker's Postgres
 * state, so it may only _exit. Set up as helper.c sets up chdb_helper, by
 * spawn.c.
 */
static void
exec_engine(int fd, int page_fd, char* const argv[]) {
    /* dup2 would close the other's target were it sitting there. */
    if (page_fd == CHDB_SEARCH_ENGINE_FD) {
        page_fd = dup(page_fd);
    }
    if (!chdb_spawn_place_fd(fd, CHDB_SEARCH_ENGINE_FD) ||
        !chdb_spawn_place_fd(page_fd, CHDB_SEARCH_PAGE_FD)) {
        _exit(126);
    }
    /* Nothing else of the worker's reaches the engine. */
#ifdef __linux__
    close_range(CHDB_SEARCH_PAGE_FD + 1, ~0U, 0);
#else
    for (int i = CHDB_SEARCH_PAGE_FD + 1; i < 1024; i++) {
        close(i);
    }
#endif
    chdb_spawn_exec(argv, MyProcPid);
}

char*
engine_ensure(Oid dboid) {
    if (engine_pid() > 0) {
        return NULL;
    }

    char pkglib[MAXPGPATH];
    get_pkglib_path(my_exec_path, pkglib);
    char* program = psprintf("%s/%s", pkglib, CHDB_SEARCH_ENGINE_PROGRAM);

    if (access(program, X_OK) != 0) {
        return psprintf("could not execute \"%s\": %m", program);
    }

    int fd[2];
    int page[2];

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd) < 0) {
        return psprintf("could not open a channel to the chDB engine: %m");
    }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, page) < 0) {
        close(fd[0]);
        close(fd[1]);
        return psprintf("could not open a page channel to the chDB engine: %m");
    }

    char* const argv[] = {
        program,
        CppAsString2(CHDB_SEARCH_ENGINE_FD),
        CppAsString2(CHDB_SEARCH_PAGE_FD),
        psprintf("%s/" CHDB_SEARCH_DIR "/%u", DataDir, dboid),
        NULL,
    };

    /* Postgres buffers would otherwise be flushed twice, once by each side. */
    fflush(NULL);
    pid_t pid = fork();

    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        close(page[0]);
        close(page[1]);
        return psprintf("could not fork the chDB engine: %m");
    }
    if (pid == 0) {
        exec_engine(fd[1], page[1], argv);
    }
    close(fd[1]);
    close(page[1]);
    engine_attach(pid, fd[0], page[0]);

    return NULL;
}
