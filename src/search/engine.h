#ifndef CHDB_SEARCH_ENGINE_H
#define CHDB_SEARCH_ENGINE_H

/*
 * The chDB session of the worker: libchdb loaded by dlopen, the store it
 * opens, and the statements run against it.
 */

#include "postgres.h"

#include "chdb.h"

#include "../setup.h"

/* Native is the only format either direction crosses in. */
static const char native_format[] = "Native";

#define CHDB_SEARCH_SYMBOLS(X)                                                         \
    X(chdb_connect)                                                                    \
    X(chdb_close_conn)                                                                 \
    X(chdb_query_n)                                                                    \
    X(chdb_stream_query_n)                                                             \
    X(chdb_stream_fetch_result)                                                        \
    X(chdb_stream_cancel_query)                                                        \
    X(chdb_stream_insert_n)                                                            \
    X(chdb_stream_append)                                                              \
    X(chdb_stream_done)                                                                \
    X(chdb_stream_cancel_insert)                                                       \
    X(chdb_stream_insert_error)                                                        \
    X(chdb_result_buffer)                                                              \
    X(chdb_result_length)                                                              \
    X(chdb_result_error)                                                               \
    X(chdb_destroy_query_result)                                                       \
    X(chdb_destroy_insert_stream)                                                      \
    X(chdb_set_signal_handlers_enabled)

/* Pointer per entry point, typed from chdb.h so a signature drift fails to build. */
struct chdbApi {
#define X(name) __typeof__(&name) name;
    CHDB_SEARCH_SYMBOLS(X)
#undef X
};

extern struct chdbApi chdb_search_api;
extern chdb_connection* chdb_search_conn;

extern void
chdb_search_load_libchdb(void);

/* Opens the store of `dboid` and configures the session. */
extern void
chdb_search_open_store(Oid dboid);

extern void
chdb_search_close_store(void);

/*
 * Runs a statement, buffering its result. NULL on success, else chDB's error
 * text as it said it, palloc'd.
 */
extern char*
chdb_search_run_statement(const char* sql, size_t len);

/* Applies the limits of `ctx` to the session, where they differ from the last. */
extern char*
chdb_search_apply_settings(const chdbHelperContext* ctx);

#endif /* CHDB_SEARCH_ENGINE_H */
