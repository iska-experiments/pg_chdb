/*
 * Running the commands of ../protocol.h against chDB: statements, streamed
 * selects and streamed inserts.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "session.h"

/* Sends the status and frees the error. */
static bool
reply(int fd, char* err) {
    bool ok = io_send_status(fd, err);

    free(err);

    return ok;
}

bool
command_exec(int fd, const chdbSearchRequest* req) {
    char* err = session_prepare(req);

    if (!err) {
        err = session_run(req->query.data, req->query.len);
    }

    return reply(fd, err);
}

bool
command_drop(int fd, const chdbSearchRequest* req) {
    char* err = session_begin(req);

    if (!err) {
        char* sql = NULL;

        if (asprintf(&sql, "DROP DATABASE IF EXISTS idx_%" PRIu32 " SYNC", req->index) <
            0) {
            err = strdup("out of memory");
        } else {
            err = session_run(sql, strlen(sql));
            free(sql);
        }
    }

    return reply(fd, err);
}

bool
command_select(int fd, const chdbSearchRequest* req) {
    char* err = session_prepare(req);
    bool lost = false;

    if (!err) {
        chdb_result* stream = chdb_stream_query_n(
            *session_conn,
            req->query.data,
            req->query.len,
            native_format,
            sizeof(native_format) - 1
        );
        const char* stream_err = chdb_result_error(stream);

        if (stream_err) {
            err = strdup(stream_err);
        }

        while (!err && !lost) {
            chdb_result* chunk    = chdb_stream_fetch_result(*session_conn, stream);
            const char* chunk_err = chdb_result_error(chunk);
            size_t len            = chunk_err ? 0 : chdb_result_length(chunk);

            if (chunk_err) {
                err = strdup(chunk_err);
            } else if (len == 0) {
                chdb_destroy_query_result(chunk);
                break; /* end of stream */
            } else if (!io_send_chunks(fd, chdb_result_buffer(chunk), len)) {
                lost = true;
            }
            chdb_destroy_query_result(chunk);
        }

        if (err || lost) {
            chdb_stream_cancel_query(*session_conn, stream);
        }
        chdb_destroy_query_result(stream);
    }

    /* A failure after data was sent still ends the data and reports itself. */
    if (lost || !io_send_end(fd)) {
        free(err);
        return false;
    }

    return reply(fd, err);
}

bool
command_insert(int fd, const chdbSearchRequest* req) {
    char* err                 = session_prepare(req);
    chdb_insert_stream stream = NULL;
    bool live                 = false;
    char* buf                 = NULL;
    size_t cap                = 0;
    bool in_step              = true;

    if (!err) {
        stream = chdb_stream_insert_n(
            *session_conn,
            req->query.data,
            req->query.len,
            native_format,
            sizeof(native_format) - 1
        );
        const char* open_err = chdb_stream_insert_error(stream);

        if (open_err) {
            err = strdup(open_err);
        } else {
            live = true;
        }
    }

    /* Read to the end-of-data chunk even after a failure, to stay in step. */
    for (;;) {
        uint32_t len;

        if (io_recv(fd, &len, sizeof(len)) != 1 || len > CHDB_SEARCH_CHUNK_MAX) {
            in_step = false;
            break;
        }
        if (len == 0) {
            break;
        }
        if (len > cap) {
            free(buf);
            cap = len;
            buf = malloc(cap);
        }
        if (!buf || io_recv(fd, buf, len) != 1) {
            in_step = false;
            break;
        }
        if (live && chdb_stream_append(stream, buf, len) != CHDBSuccess) {
            const char* append_err = chdb_stream_insert_error(stream);

            err = strdup(append_err ? append_err : "could not append to chDB");
            chdb_stream_cancel_insert(stream);
            live = false;
        }
    }
    free(buf);

    if (stream) {
        if (live && !in_step) {
            chdb_stream_cancel_insert(stream);
        } else if (live) {
            chdb_result* done    = chdb_stream_done(stream);
            const char* done_err = chdb_result_error(done);

            if (done_err) {
                err = strdup(done_err);
            }
            chdb_destroy_query_result(done);
        }
        chdb_destroy_insert_stream(stream);
    }

    if (!in_step) {
        free(err);
        return false;
    }

    return reply(fd, err);
}
