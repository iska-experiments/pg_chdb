/*
 * The libchdb session: opening the store, settings, and running statements.
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session.h"

chdb_connection* session_conn;

/* The settings the session carries now, so a request changes them only on need. */
static int applied_memory  = -1;
static int applied_threads = -1;
static int applied_parsers = -1;

/*
 * Minus the Request ID line and chDB version that differ between runs, as
 * helper.c does.
 */
char*
session_clean_error(const char* raw) {
    char* msg  = strdup(raw);
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
session_run(const char* sql, size_t len) {
    chdb_result* res =
        chdb_query_n(*session_conn, sql, len, native_format, sizeof(native_format) - 1);
    const char* err = chdb_result_error(res);
    char* out       = err ? session_clean_error(err) : NULL;

    chdb_destroy_query_result(res);

    return out;
}

/* Settings are per session and arguments to chdb_connect do not take them. */
static char*
apply_settings(int memory, int threads, int parsers) {
    if (memory == applied_memory && threads == applied_threads &&
        parsers == applied_parsers) {
        return NULL;
    }

    char* sql = NULL;

    if (asprintf(
            &sql,
            "SET allow_experimental_nullable_tuple_type,"
            "output_format_json_quote_denormals,"
            "output_format_native_write_json_as_string,"
            "output_format_native_encode_types_in_binary_format=0,"
            "date_time_output_format='iso',"
            "max_threads=%d,max_parsing_threads=%d,max_memory_usage=%" PRIu64,
            threads,
            parsers,
            (uint64_t)memory * 1024 * 1024
        ) < 0) {
        return strdup("out of memory");
    }

    char* err = session_run(sql, strlen(sql));

    if (!err) {
        applied_memory  = memory;
        applied_threads = threads;
        applied_parsers = parsers;
    }
    free(sql);

    return err;
}

char*
session_apply_settings(const request* req) {
    return apply_settings(req->max_memory, req->max_threads, req->max_parsers);
}

/*
 * The indexes whose database and meta table this engine has made. It is the
 * only process on the store, and a database goes only through command_drop,
 * so the DDL need run once per index; a full list only costs repeats.
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
session_forget(uint32_t index) {
    for (size_t i = 0; i < nprepared; i++) {
        if (prepared[i] == index) {
            prepared[i] = prepared[--nprepared];
            return;
        }
    }
}

/* Makes the index's database and meta table, once. */
static char*
prepare_database(uint32_t index) {
    static const char* const ddl[] = {
        "CREATE DATABASE IF NOT EXISTS " CHDB_STORE_DB_FMT, CHDB_STORE_META_DDL
    };

    if (is_prepared(index)) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(ddl) / sizeof(ddl[0]); i++) {
        char* sql = NULL;

        if (asprintf(&sql, ddl[i], index) < 0) {
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
table_exists(const request* req, bool* exists) {
    char* sql = NULL;

    if (asprintf(
            &sql, "EXISTS TABLE " CHDB_STORE_TABLE_FMT, req->index, req->generation
        ) < 0) {
        return strdup("out of memory");
    }

    chdb_result* res = chdb_query_n(*session_conn, sql, strlen(sql), "TSV", 3);
    const char* err  = chdb_result_error(res);
    char* out        = err ? session_clean_error(err) : NULL;

    *exists = !err && chdb_result_length(res) > 0 && chdb_result_buffer(res)[0] == '1';
    chdb_destroy_query_result(res);
    free(sql);

    return out;
}

char*
session_prepare(const request* req, bool* no_store) {
    char* err   = session_apply_settings(req);
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

void
session_open(const char* path) {
    char* arg = NULL;

    if (asprintf(&arg, "--path=%s", path) < 0) {
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
