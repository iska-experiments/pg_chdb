#ifndef CHDB_SEARCH_ENGINE_SESSION_H
#define CHDB_SEARCH_ENGINE_SESSION_H

/* The engine's one libchdb connection, held open for the life of the process. */

#include "chdb.h"

#include "../protocol.h"

/* Native is the only format either direction crosses in. */
static const char native_format[] = "Native";

extern chdb_connection* session_conn;

/* Opens the store at `path`, exiting with a message on stderr if it cannot. */
extern void
session_open(const char* path);

extern void
session_close(void);

/*
 * Runs a statement, buffering its result. NULL on success, else chDB's error
 * text as it said it, malloc'd; the backend scrubs it.
 */
extern char*
session_run(const char* sql, size_t len);

/*
 * Readies the session for `req`: refuses what the engine cannot run, then
 * applies the request's limits where they differ from the last.
 */
extern char*
session_begin(const chdbSearchRequest* req);

/* session_begin, then the index's database, created if it is missing. */
extern char*
session_prepare(const chdbSearchRequest* req);

#endif /* CHDB_SEARCH_ENGINE_SESSION_H */
