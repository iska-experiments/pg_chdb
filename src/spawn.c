/*
 * Forking a chDB child process with the descriptors it is handed and the exec
 * preamble it runs, shared by every module that starts one. See spawn.h.
 */

#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "miscadmin.h"

#include "spawn.h"

char*
chdb_spawn_path(const char* program) {
    char pkglib[MAXPGPATH];

    get_pkglib_path(my_exec_path, pkglib);
    return psprintf("%s/%s", pkglib, program);
}

/*
 * In the child, between the fork and the exec, so only async-signal-safe
 * calls. Every descriptor is first lifted clear of the targets, so that
 * placing one never closes the source of another, then put in place, which
 * clears close-on-exec; everything above the last target is closed.
 */
static void
place_fds(const int* fds, int nfds) {
    int high[CHDB_SPAWN_MAX_FDS];

    for (int i = 0; i < nfds; i++) {
        int fd = fds[i] < 0 ? open("/dev/null", O_RDWR) : fds[i];

        high[i] = fd < 0 ? -1 : fcntl(fd, F_DUPFD, nfds);
        if (high[i] < 0) {
            _exit(126);
        }
    }
    for (int i = 0; i < nfds; i++) {
        if (dup2(high[i], i) != i) {
            _exit(126);
        }
    }
#ifdef __linux__
    close_range(nfds, ~0U, 0);
#else
    for (int i = nfds; i < 1024; i++) {
        close(i);
    }
#endif
}

pid_t
chdb_spawn(char* const argv[], const int* fds, int nfds) {
    pid_t parent = getpid();
    pid_t pid;
    sigset_t none;

    Assert(nfds >= 0 && nfds <= CHDB_SPAWN_MAX_FDS);
    pid = fork();
    if (pid != 0) {
        return pid;
    }

    place_fds(fds, nfds);
    /* Postgres ignores SIGPIPE; ClickHouse wants the default disposition. */
    signal(SIGPIPE, SIG_DFL);
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
#ifdef __linux__
    /* Die with the parent, whose channel or store lock a successor needs free. */
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    if (getppid() != parent) {
        _exit(1); /* the parent went before the prctl took hold */
    }

    execv(argv[0], argv);
    _exit(127);
}
