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

#include "../channel.h"

/* Starts the engine for database `dboid` unless it runs. NULL, or why not. */
extern char*
engine_ensure(Oid dboid);

/* engine_spawn.c hands over the child it forked: its pid and the worker's socket ends.
 */
extern void
engine_attach(pid_t pid, int fd, int page_fd);

/* The engine's pid, or 0 when none runs. */
extern pid_t
engine_pid(void);

/*
 * The channel to a running engine, NULL when none runs, for a request the
 * worker makes itself. Its reads raise on a broken engine, unlike the pair
 * below; engine_death then puts the engine down.
 */
extern chdbChannel*
engine_channel(void);

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

/*
 * The engine's page channel, on which it asks for its blobs (see
 * pagestore/pagestore.h): its descriptor, for the event loop to watch, -1
 * when there is none; and the answering of one request when it is readable,
 * false when the channel broke, which an engine does not survive.
 */
extern int
engine_page_fd(void);

extern bool
engine_serve_page(void);

#endif /* CHDB_SEARCH_ENGINE_PROC_H */
