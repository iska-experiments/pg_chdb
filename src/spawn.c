/*
 * The descriptors a chDB child process is handed and the exec preamble it
 * runs, shared by every module that forks one. See spawn.h.
 */

#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "spawn.h"

bool
chdb_spawn_place_fd(int fd, int target) {
    return fd == target ? fcntl(fd, F_SETFD, 0) == 0 : dup2(fd, target) == target;
}

void
chdb_spawn_exec(char* const argv[], pid_t parent) {
    sigset_t none;

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
