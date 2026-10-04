#ifndef CHDB_SEARCH_ENGINE_PROC_H
#define CHDB_SEARCH_ENGINE_PROC_H

/*
 * The worker's supervision of its chdb_search_engine child: starting it on
 * demand, trading bytes with it, and saying how it died.
 *
 * Failures here are returned, not raised, as the caller owes the client a
 * status frame whatever happens to the engine.
 */

#include "postgres.h"

/* Starts the engine for database `dboid` unless it runs. NULL, or why not. */
extern char*
engine_ensure(Oid dboid);

/* The engine's pid, or 0 when none runs. */
extern pid_t
engine_pid(void);

/* Both false when the engine has gone; call engine_death to learn how. */
extern bool
engine_send(const void* buf, size_t len);

extern bool
engine_recv(void* buf, size_t len);

/*
 * Reaps the engine and describes how it ended, in the words of helper.c, and
 * logs it. Also the way to drop an engine whose framing cannot be trusted.
 */
extern char*
engine_death(void);

/* Lets an idle engine close its store and exit, killing it if it will not. */
extern void
engine_stop(void);

#endif /* CHDB_SEARCH_ENGINE_PROC_H */
