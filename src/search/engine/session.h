#ifndef CHDB_SEARCH_ENGINE_SESSION_H
#define CHDB_SEARCH_ENGINE_SESSION_H

/* The engine's one libchdb connection, held open for the life of the process. */

#include "chdb.h"

#include "io.h"

/* Native is the only format either direction crosses in. */
static const char native_format[] = "Native";

extern chdb_connection* session_conn;

/* Opens the store at `path`, exiting with a message on stderr if it cannot. */
extern void
session_open(const char* path);

extern void
session_close(void);

/* The error as a client should see it, malloc'd. */
extern char*
session_clean_error(const char* raw);

/* Runs a statement, buffering its result. NULL on success, else a malloc'd error. */
extern char*
session_run(const char* sql, size_t len);

/*
 * Settings, database and meta table every statement against an index needs
 * first, and, for a request naming a generation, that its table exists:
 * `*no_store` is set with the error when it does not.
 */
extern char*
session_prepare(const request* req, bool* no_store);

/* The index's database was dropped, so its next request makes it again. */
extern void
session_forget(uint32_t index);

/* Applies the request's settings, for commands that do not need the database. */
extern char*
session_apply_settings(const request* req);

#endif /* CHDB_SEARCH_ENGINE_SESSION_H */
