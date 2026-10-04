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

/*
 * close_range came with glibc 2.34 and Linux 5.9. Without it, or when the
 * kernel lacks it, the child closes descriptors one by one up to the open
 * file limit, read before the fork as sysconf is not async-signal-safe, and
 * capped, as the limit can run to millions.
 */
#if defined(__linux__) && defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34)
#define CHDB_HAVE_CLOSE_RANGE 1
#endif
#endif
#define CHDB_SPAWN_CLOSE_CAP 65536

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
place_fds(const int* fds, int nfds, int open_max) {
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
#ifdef CHDB_HAVE_CLOSE_RANGE
    if (close_range(nfds, ~0U, 0) == 0) {
        return;
    }
#endif
    for (int i = nfds; i < open_max; i++) {
        close(i);
    }
}

pid_t
chdb_spawn(char* const argv[], const int* fds, int nfds) {
    pid_t parent  = getpid();
    long open_max = sysconf(_SC_OPEN_MAX);
    pid_t pid;
    sigset_t none;

    Assert(nfds >= 0 && nfds <= CHDB_SPAWN_MAX_FDS);
    if (open_max < 0 || open_max > CHDB_SPAWN_CLOSE_CAP) {
        open_max = CHDB_SPAWN_CLOSE_CAP;
    }
    pid = fork();
    if (pid != 0) {
        return pid;
    }

    place_fds(fds, nfds, (int)open_max);
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
