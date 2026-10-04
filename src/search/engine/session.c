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

char*
session_prepare(const request* req) {
    char* err = session_apply_settings(req);
    char* sql = NULL;

    if (err) {
        return err;
    }
    if (asprintf(&sql, "CREATE DATABASE IF NOT EXISTS idx_%u", req->index) < 0) {
        return strdup("out of memory");
    }
    err = session_run(sql, strlen(sql));
    free(sql);

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
