/*
 * Running the commands of protocol.h against chDB: statements, streamed
 * selects and streamed inserts.
 */

#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/interrupt.h"

#include "commands.h"
#include "engine.h"
#include "framing.h"

/* Settings and database every statement against an index needs first. */
static char*
prepare(const request* req) {
    char* err = chdb_search_apply_settings(&req->ctx);

    if (err) {
        return err;
    }

    char* sql = psprintf("CREATE DATABASE IF NOT EXISTS idx_%u", req->index);
    err       = chdb_search_run_statement(sql, strlen(sql));
    pfree(sql);

    return err;
}

bool
command_exec(int fd, const request* req) {
    char* err = prepare(req);

    if (!err) {
        err = chdb_search_run_statement(req->query.data, req->query.len);
    }

    return frame_send_status(fd, err);
}

bool
command_drop(int fd, const request* req) {
    char* err = chdb_search_apply_settings(&req->ctx);

    if (!err) {
        char* sql = psprintf("DROP DATABASE IF EXISTS idx_%u SYNC", req->index);
        err       = chdb_search_run_statement(sql, strlen(sql));
        pfree(sql);
    }

    return frame_send_status(fd, err);
}

bool
command_select(int fd, const request* req) {
    char* err = prepare(req);
    bool lost = false;

    if (!err) {
        chdb_result* stream = chdb_search_api.chdb_stream_query_n(
            *chdb_search_conn,
            req->query.data,
            req->query.len,
            native_format,
            sizeof(native_format) - 1
        );
        const char* stream_err = chdb_search_api.chdb_result_error(stream);

        if (stream_err) {
            err = pstrdup(stream_err);
        }

        while (!err && !lost) {
            CHECK_FOR_INTERRUPTS();
            if (ShutdownRequestPending) {
                lost = true;
                break;
            }

            chdb_result* chunk =
                chdb_search_api.chdb_stream_fetch_result(*chdb_search_conn, stream);
            const char* chunk_err = chdb_search_api.chdb_result_error(chunk);
            size_t len = chunk_err ? 0 : chdb_search_api.chdb_result_length(chunk);

            if (chunk_err) {
                err = pstrdup(chunk_err);
            } else if (len == 0) {
                chdb_search_api.chdb_destroy_query_result(chunk);
                break; /* end of stream */
            } else if (!frame_send_chunks(
                           fd, chdb_search_api.chdb_result_buffer(chunk), len
                       )) {
                lost = true;
            }
            chdb_search_api.chdb_destroy_query_result(chunk);
        }

        if (err || lost) {
            chdb_search_api.chdb_stream_cancel_query(*chdb_search_conn, stream);
        }
        chdb_search_api.chdb_destroy_query_result(stream);
    }

    /* A failure after data was sent still ends the data and reports itself. */
    return !lost && frame_send_end(fd) && frame_send_status(fd, err);
}

bool
command_insert(int fd, const request* req) {
    char* err                 = prepare(req);
    chdb_insert_stream stream = NULL;
    bool live                 = false;
    char* buf                 = NULL;
    size_t cap                = 0;
    bool in_step              = true;

    if (!err) {
        stream = chdb_search_api.chdb_stream_insert_n(
            *chdb_search_conn,
            req->query.data,
            req->query.len,
            native_format,
            sizeof(native_format) - 1
        );
        const char* open_err = chdb_search_api.chdb_stream_insert_error(stream);

        if (open_err) {
            err = pstrdup(open_err);
        } else {
            live = true;
        }
    }

    /* Read to the end-of-data chunk even after a failure, to stay in step. */
    for (;;) {
        uint32_t len;

        if (frame_recv(fd, &len, sizeof(len)) != 1 || len > CHDB_CHANNEL_CHUNK_MAX) {
            in_step = false;
            break;
        }
        if (len == 0) {
            break;
        }
        if (len > cap) {
            if (buf) {
                pfree(buf);
            }
            cap = len;
            buf = palloc(cap);
        }
        if (frame_recv(fd, buf, len) != 1) {
            in_step = false;
            break;
        }
        if (live &&
            chdb_search_api.chdb_stream_append(stream, buf, len) != CHDBSuccess) {
            const char* append_err = chdb_search_api.chdb_stream_insert_error(stream);

            err = pstrdup(append_err ? append_err : "could not append to chDB");
            chdb_search_api.chdb_stream_cancel_insert(stream);
            live = false;
        }
    }

    if (stream) {
        if (live && !in_step) {
            chdb_search_api.chdb_stream_cancel_insert(stream);
        } else if (live) {
            chdb_result* done    = chdb_search_api.chdb_stream_done(stream);
            const char* done_err = chdb_search_api.chdb_result_error(done);

            if (done_err) {
                err = pstrdup(done_err);
            }
            chdb_search_api.chdb_destroy_query_result(done);
        }
        chdb_search_api.chdb_destroy_insert_stream(stream);
    }

    return in_step && frame_send_status(fd, err);
}
