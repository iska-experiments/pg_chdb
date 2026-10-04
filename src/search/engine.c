/*
 * The chDB session of the worker. libchdb is loaded by dlopen since the
 * backends must not link it. See engine.h.
 */

#include "postgres.h"

#include <dlfcn.h>

#include "miscadmin.h"

#include "engine.h"
#include "worker.h"

struct chdbApi chdb_search_api;
chdb_connection* chdb_search_conn;

/* The limits the session carries now, so a request changes them only on need. */
static chdbHelperContext applied;
static bool applied_any;

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

char*
chdb_search_run_statement(const char* sql, size_t len) {
    chdb_result* res = chdb_search_api.chdb_query_n(
        *chdb_search_conn, sql, len, native_format, sizeof(native_format) - 1
    );
    const char* err = chdb_search_api.chdb_result_error(res);
    char* out       = err ? pstrdup(err) : NULL;

    chdb_search_api.chdb_destroy_query_result(res);

    return out;
}

/* Settings are per session and arguments to chdb_connect do not take them. */
char*
chdb_search_apply_settings(const chdbHelperContext* ctx) {
    if (applied_any && ctx->max_memory == applied.max_memory &&
        ctx->max_threads == applied.max_threads &&
        ctx->max_parsers == applied.max_parsers) {
        return NULL;
    }

    char* sql = psprintf(
        CHDB_SESSION_SETTINGS_FMT,
        ctx->max_threads,
        ctx->max_parsers,
        CHDB_SESSION_MEMORY_BYTES(ctx->max_memory)
    );
    char* err = chdb_search_run_statement(sql, strlen(sql));

    if (!err) {
        applied     = *ctx;
        applied_any = true;
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

    chdbHelperContext ctx = {
        .max_memory  = (uint16_t)chdb_max_memory,
        .max_threads = (uint16_t)chdb_max_threads,
        .max_parsers = (uint16_t)chdb_max_parsers,
    };
    char* err = chdb_search_apply_settings(&ctx);
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
