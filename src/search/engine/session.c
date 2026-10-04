/*
 * The libchdb session: opening the store, settings, and running statements.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session.h"

chdb_connection* session_conn;

/* The limits the session carries now, so a request changes them only on need. */
static chdbHelperContext applied;
static bool applied_any;

char*
session_run(const char* sql, size_t len) {
    chdb_result* res =
        chdb_query_n(*session_conn, sql, len, native_format, sizeof(native_format) - 1);
    const char* err = chdb_result_error(res);
    char* out       = err ? strdup(err) : NULL;

    chdb_destroy_query_result(res);

    return out;
}

/* Settings are per session and arguments to chdb_connect do not take them. */
static char*
apply_settings(const chdbHelperContext* ctx) {
    if (applied_any && ctx->max_memory == applied.max_memory &&
        ctx->max_threads == applied.max_threads &&
        ctx->max_parsers == applied.max_parsers) {
        return NULL;
    }

    char* sql = NULL;

    if (asprintf(
            &sql,
            CHDB_SESSION_SETTINGS_FMT,
            ctx->max_threads,
            ctx->max_parsers,
            CHDB_SESSION_MEMORY_BYTES(ctx->max_memory)
        ) < 0) {
        return strdup("out of memory");
    }

    char* err = session_run(sql, strlen(sql));

    if (!err) {
        applied     = *ctx;
        applied_any = true;
    }
    free(sql);

    return err;
}

char*
session_begin(const chdbSearchRequest* req) {
    if (req->nparams) {
        return strdup("query parameters are not supported");
    }

    return apply_settings(&req->ctx);
}

/*
 * Whether the table of `req`'s generation exists. The one-line text answer
 * of EXISTS is read rather than a Native block.
 */
static char*
table_exists(const chdbSearchRequest* req, bool* exists) {
    char* sql = NULL;

    if (asprintf(
            &sql, "EXISTS TABLE " CHDB_STORE_TABLE_FMT, req->index, req->generation
        ) < 0) {
        return strdup("out of memory");
    }

    chdb_result* res = chdb_query_n(*session_conn, sql, strlen(sql), "TSV", 3);
    const char* err  = chdb_result_error(res);
    char* out        = err ? strdup(err) : NULL;

    *exists = !err && chdb_result_length(res) > 0 && chdb_result_buffer(res)[0] == '1';
    chdb_destroy_query_result(res);
    free(sql);

    return out;
}

char*
session_prepare(const chdbSearchRequest* req, bool* no_store) {
    char* err   = session_begin(req);
    char* sql   = NULL;
    bool exists = false;

    *no_store = false;
    if (err) {
        return err;
    }
    if (asprintf(&sql, "CREATE DATABASE IF NOT EXISTS " CHDB_STORE_DB_FMT, req->index) <
        0) {
        return strdup("out of memory");
    }
    err = session_run(sql, strlen(sql));
    free(sql);
    if (err || req->generation == 0) {
        return err;
    }

    /* A backend names the generation its metapage holds; the store must have it. */
    err = table_exists(req, &exists);
    if (!err && !exists) {
        *no_store = true;
        if (asprintf(
                &err,
                "the store has no table " CHDB_STORE_TABLE_FMT,
                req->index,
                req->generation
            ) < 0) {
            err = strdup("out of memory");
        }
    }

    return err;
}

void
session_open(const char* path) {
    char* arg = NULL;

    if (asprintf(&arg, "--path=%s", path) < 0) {
        fprintf(stderr, "chdb_search: out of memory\n");
        exit(1);
    }
    session_conn = chdb_connect(2, (char*[]){ "chdb", arg, NULL });
    if (!session_conn || !*session_conn) {
        fprintf(stderr, "chdb_search: could not open the chDB store at %s\n", path);
        exit(1);
    }
    free(arg);
}

void
session_close(void) {
    if (session_conn) {
        chdb_close_conn(session_conn);
        session_conn = NULL;
    }
}
