#ifndef CHDB_SPAWN_H
#define CHDB_SPAWN_H

#include "postgres.h"

/*
 * Starting a process that runs chDB: chdb_helper for a COPY (helper.c), or a
 * program of another extension's, such as a search engine its background
 * worker forks. Each is forked from a process holding Postgres state, which
 * the child may not touch before its exec, and each wants the same state
 * from it, so that setup lives here once.
 */

/* The most descriptors a child can be handed. */
#define CHDB_SPAWN_MAX_FDS 16

/*
 * The path of `program` in pkglibdir, where an extension installs the
 * programs it forks, palloc'd. Whether it can be executed is the caller's to
 * check, as only the caller knows what to say when it cannot.
 */
extern char*
chdb_spawn_path(const char* program);

/*
 * Forks a child that execs `argv`, argv[0] the program's path, with fds[i]
 * as its descriptor i for every i below `nfds`, /dev/null for a negative
 * one, and no other descriptor of the caller's open. Before the exec the child takes
 * the state ClickHouse wants to run in: SIGPIPE at its default, no signal blocked, and
 * dying with the caller, which is checked once that is arranged so that a caller gone
 * already leaves no orphan. The caller keeps its own copies of `fds` to
 * close. Returns the child's pid, or -1 with errno set when the fork fails;
 * the child _exits 126 when it cannot take its descriptors and 127 when the
 * exec fails. Flush stdio first, or the child flushes the caller's buffers
 * a second time.
 */
extern pid_t
chdb_spawn(char* const argv[], const int* fds, int nfds);

#endif /* CHDB_SPAWN_H */
