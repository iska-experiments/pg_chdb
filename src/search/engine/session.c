/*
 * The libchdb session: opening the store, settings, and running statements.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../pagestore/protocol.h"
#include "pagestore.h"
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
 * The indexes whose database and meta table this engine has made. It is the
 * only process on the store, and a database goes only through a drop, which
 * forgets them all, so the DDL need run once per index; a full list only
 * costs repeats.
 */
static uint32_t prepared[128];
static size_t nprepared;

static bool
is_prepared(uint32_t index) {
    for (size_t i = 0; i < nprepared; i++) {
        if (prepared[i] == index) {
            return true;
        }
    }

    return false;
}

void
session_forget(void) {
    nprepared = 0;
}

/* Registers the index's blob storage and makes its database and meta table, once. */
static char*
prepare_database(uint32_t index) {
    static const char* const ddl[] = {
        "CREATE DATABASE IF NOT EXISTS " CHDB_STORE_DB_FMT, CHDB_STORE_META_DDL
    };
    char storage[CHDB_PAGE_STORAGE_MAX];

    if (is_prepared(index)) {
        return NULL;
    }
    /* Before any table is made on it; the tables persisted were registered at start. */
    snprintf(storage, sizeof(storage), CHDB_STORE_STORAGE_FMT, index);
    if (!pagestore_register(storage)) {
        return strdup("could not register the index's blob storage");
    }
    for (size_t i = 0; i < sizeof(ddl) / sizeof(ddl[0]); i++) {
        char* sql = NULL;

        if (asprintf(&sql, ddl[i], index, index) < 0) {
            return strdup("out of memory");
        }

        char* err = session_run(sql, strlen(sql));

        free(sql);
        if (err) {
            return err;
        }
    }
    if (nprepared < sizeof(prepared) / sizeof(prepared[0])) {
        prepared[nprepared++] = index;
    }

    return NULL;
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
    bool exists = false;

    *no_store = false;
    if (!err) {
        err = prepare_database(req->index);
    }
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

/*
 * The callback disk has no hard links, so a DELETE can only patch parts, as
 * VACUUM's do (vacuum.c); the session's default makes any other DELETE, a
 * debug function's say, do the same instead of failing as a mutation.
 */
#define CHDB_SESSION_DELETE_MODE "--lightweight_delete_mode=lightweight_update_force"

void
session_open(const char* path) {
    char* arg = NULL;

    if (asprintf(&arg, "--path=%s", path) < 0) {
        fprintf(stderr, "chdb_search: out of memory\n");
        exit(1);
    }
    session_conn =
        chdb_connect(3, (char*[]){ "chdb", arg, CHDB_SESSION_DELETE_MODE, NULL });
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
