#ifndef CHDB_SPAWN_H
#define CHDB_SPAWN_H

#include "postgres.h"

/*
 * Starting a process that runs chDB: chdb_helper for a COPY (helper.c) or
 * chdb_search_engine for a search worker. Each is forked from a process
 * holding Postgres state, which the child may not touch before its exec, and
 * both want the same state from it, so that setup lives here once.
 */

/*
 * Puts `fd` at `target` for the program about to be exec'd, which also clears
 * close-on-exec. A descriptor already in place only needs to stay open.
 */
extern bool
chdb_spawn_place_fd(int fd, int target);

/*
 * Execs `argv` in a child forked by `parent`, as ClickHouse wants to run:
 * SIGPIPE at its default, no signal blocked, and dying with the parent, which
 * is checked once that is arranged so that a parent gone already leaves no
 * orphan. Does not return: the child _exits when it cannot exec.
 */
extern void
chdb_spawn_exec(char* const argv[], pid_t parent);

#endif /* CHDB_SPAWN_H */
