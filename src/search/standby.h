#ifndef CHDB_SEARCH_STANDBY_H
#define CHDB_SEARCH_STANDBY_H

/*
 * The worker on a server in recovery, and its promotion (standby.c). The
 * index pages are replayed from the primary, so a standby serves searches
 * from them with an engine that writes nothing; when the server is
 * promoted the engine restarts read-write, and nothing is rebuilt.
 */

#include "postgres.h"

/* Before the worker listens: notes whether the server is in recovery. */
extern void
chdb_search_standby_init(void);

/* Milliseconds the event loop may wait: bounded in recovery, else forever. */
extern long
chdb_search_standby_timeout(void);

/* Once per loop: a server found promoted since init restarts its engine. */
extern void
chdb_search_standby_poll(void);

#endif /* CHDB_SEARCH_STANDBY_H */
