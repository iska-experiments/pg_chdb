/*
 * The chDB session of the worker. libchdb is loaded by dlopen since the
 * backends must not link it. See engine.h.
 */

#include "postgres.h"

#include <dlfcn.h>
#include <inttypes.h>

#include "miscadmin.h"
#include "utils/guc.h"

#include "engine.h"
#include "worker.h"

struct chdbApi chdb_search_api;
chdb_connection* chdb_search_conn;

/* The settings the chDB session carries now, so a request changes them only on need. */
static int applied_memory  = -1;
static int applied_threads = -1;
static int applied_parsers = -1;

void
chdb_search_load_libchdb(void) {
    void* lib = dlopen(chdb_search_libchdb_path, RTLD_NOW | RTLD_LOCAL);

    if (!lib) {
        ereport(
            FATAL,
            errcode(ERRCODE_UNDEFINED_FILE),
            errmsg("chdb_search: could not load \"%s\"", chdb_search_libchdb_path),
            errdetail("%s", dlerror()),
            errhint("Set chdb_search.libchdb_path or LD_LIBRARY_PATH for the server.")
        );
    }

#define X(name)                                                                        \
    if (!(*(void**)& chdb_search_api.name = dlsym(lib, #name))) {                      \
        ereport(                                                                       \
            FATAL,                                                                     \
            errcode(ERRCODE_UNDEFINED_FUNCTION),                                       \
            errmsg("chdb_search: \"%s\" lacks %s", chdb_search_libchdb_path, #name),   \
            errdetail("%s", dlerror())                                                 \
        );                                                                             \
    }
    CHDB_SEARCH_SYMBOLS(X)
#undef X
}

/*
 * The error as a client should see it, minus the Request ID line and chDB
 * version that differ between runs, as helper.c does.
 */
char*
chdb_search_clean_error(const char* raw) {
    char* msg  = pstrdup(raw);
    size_t len = strlen(msg);

    while (len && (msg[len - 1] == '\n' || msg[len - 1] == '\r')) {
        msg[--len] = '\0';
    }

    const char* id  = strstr(msg, "Request ID:");
    const char* eol = id ? strchr(id, '\n') : NULL;
    if (eol) {
        memmove((char*)id, eol + 1, strlen(eol + 1) + 1);
    }

    const char* version = strstr(msg, " (version ");
    const char* close   = version ? strchr(version, ')') : NULL;
    if (close) {
        memmove((char*)version, close + 1, strlen(close + 1) + 1);
    }

    return msg;
}

char*
chdb_search_run_statement(const char* sql, size_t len) {
    chdb_result* res = chdb_search_api.chdb_query_n(
        *chdb_search_conn, sql, len, native_format, sizeof(native_format) - 1
    );
    const char* err = chdb_search_api.chdb_result_error(res);
    char* out       = err ? chdb_search_clean_error(err) : NULL;

    chdb_search_api.chdb_destroy_query_result(res);

    return out;
}

/* Settings are per session and arguments to chdb_connect do not take them. */
char*
chdb_search_apply_settings(int memory, int threads, int parsers) {
    if (memory == applied_memory && threads == applied_threads &&
        parsers == applied_parsers) {
        return NULL;
    }

    char* sql = psprintf(
        "SET allow_experimental_nullable_tuple_type,"
        "output_format_json_quote_denormals,"
        "output_format_native_write_json_as_string,"
        "output_format_native_encode_types_in_binary_format=0,"
        "date_time_output_format='iso',"
        "max_threads=%d,max_parsing_threads=%d,max_memory_usage=%" PRIu64,
        threads,
        parsers,
        (uint64_t)memory * 1024 * 1024
    );
    char* err = chdb_search_run_statement(sql, strlen(sql));

    if (!err) {
        applied_memory  = memory;
        applied_threads = threads;
        applied_parsers = parsers;
    }
    pfree(sql);

    return err;
}

void
chdb_search_open_store(Oid dboid) {
    char* path = psprintf("--path=%s/pg_chdb/%u", DataDir, dboid);

    /*
     * chDB's handlers would run on whichever thread takes a fatal signal and
     * replace Postgres's own.
     */
    chdb_search_api.chdb_set_signal_handlers_enabled(0);
    chdb_search_conn = chdb_search_api.chdb_connect(2, (char*[]){ "chdb", path, NULL });
    if (!chdb_search_conn || !*chdb_search_conn) {
        ereport(
            FATAL,
            errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
            errmsg("chdb_search: could not open the chDB store"),
            errdetail("path: %s", path + strlen("--path="))
        );
    }
    pfree(path);

    char* err =
        chdb_search_apply_settings(chdb_max_memory, chdb_max_threads, chdb_max_parsers);
    if (err) {
        ereport(
            FATAL,
            errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
            errmsg("chdb_search: could not configure chDB"),
            errdetail("%s", err)
        );
    }
}

void
chdb_search_close_store(void) {
    if (chdb_search_conn) {
        chdb_search_api.chdb_close_conn(chdb_search_conn);
        chdb_search_conn = NULL;
    }
}
