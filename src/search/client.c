/*
 * Backend side of the chdb_search worker protocol: connecting, starting the
 * worker when it is not there, and framing requests and status replies. The
 * Native blocks in between flow through the chdbChannel of channel.h, the same
 * transport COPY uses to talk to chdb_helper. See protocol.h for the framing.
 */

#include "postgres.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "../helper.h"
#include "client.h"
#include "worker.h"

/* Milliseconds between connection attempts while the worker comes up. */
#define CHDB_SEARCH_RETRY_MS 20

struct chdbSearchConn {
    chdbChannel ch;
    chdbCmdType cmd;   /* of the request under way, so finish knows which end it is */
    const char* query; /* for error context */
};

/* Raises for a connection that broke; the stream is lost so close it. */
static void
lost_worker(chdbChannel* ch, const char* what, int errnum) {
    chdbSearchConn* conn = (chdbSearchConn*)ch;

    chdb_channel_close(ch);
    ereport(
        ERROR,
        errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
        errmsg("chdb_search: %s", what),
        errdetail(
            "The connection to the chdb_search worker was lost%s%s.",
            errnum ? ": " : "",
            errnum ? strerror(errnum) : ""
        ),
        errcontext("query: %s", conn->query ? conn->query : "")
    );
}

/* ---- connecting ---------------------------------------------------------- */

/* A connected socket, or -1 with errno saying why not. */
static int
try_connect(void) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int fd;

    /* Relative to the data directory, which backends run in. */
    snprintf(
        addr.sun_path, sizeof(addr.sun_path), CHDB_SEARCH_SOCKET_FMT, MyDatabaseId
    );
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        int saved = errno;

        close(fd);
        errno = saved;
        return -1;
    }

    return fd;
}

chdbSearchConn*
chdb_search_connect(void) {
    chdbSearchConn* conn = palloc0(sizeof(*conn));
    TimestampTz start    = GetCurrentTimestamp();

    /* Owned first, so the descriptor has an owner from the start. */
    chdb_channel_init(&conn->ch, -1, -1);
    conn->ch.chunked   = true;
    conn->ch.recv_what = "error receiving from the worker";
    conn->ch.send_what = "error sending to the worker";
    conn->ch.fail      = lost_worker;
    chdb_channel_own(&conn->ch);

    for (;;) {
        CHECK_FOR_INTERRUPTS();
        conn->ch.data = try_connect();
        if (conn->ch.data >= 0) {
            chdb_channel_prepare_fd(conn->ch.data);
            return conn;
        }

        /* No socket yet, a stale one, or a full backlog: start it and retry. */
        if (errno != ENOENT && errno != ECONNREFUSED && errno != EAGAIN) {
            ereport(
                ERROR,
                errcode_for_socket_access(),
                errmsg("chdb_search: could not connect to the worker: %m")
            );
        }
        chdb_search_worker_ensure(MyDatabaseId);

        if (TimestampDifferenceExceeds(
                start, GetCurrentTimestamp(), chdb_search_worker_timeout * 1000
            )) {
            ereport(
                ERROR,
                errcode(ERRCODE_CONNECTION_FAILURE),
                errmsg("chdb_search: timed out waiting for the worker"),
                errdetail(
                    "No worker answered within %d seconds.", chdb_search_worker_timeout
                ),
                errhint(
                    "See the server log, and that the worker finds libchdb at "
                    "chdb_search.libchdb_path."
                )
            );
        }
        WaitLatch(
            MyLatch,
            WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
            CHDB_SEARCH_RETRY_MS,
            PG_WAIT_EXTENSION
        );
        ResetLatch(MyLatch);
    }
}

void
chdb_search_close(chdbSearchConn* conn) {
    if (conn) {
        chdb_channel_close(&conn->ch);
    }
}

/* ---- requests ------------------------------------------------------------ */

/* Sends the request frame of protocol.h: its length, the index, the payload. */
static void
send_request(chdbSearchConn* conn, chdbCmdType cmd, Oid index, const char* sql) {
    StringInfoData buf;
    uint32_t len          = 0;
    chdbHelperContext ctx = {
        .cmd         = cmd,
        .max_memory  = (uint16_t)chdb_max_memory,
        .max_threads = (uint16_t)chdb_max_threads,
        .max_parsers = (uint16_t)chdb_max_parsers,
    };

    conn->cmd           = cmd;
    conn->query         = sql;
    conn->ch.chunk_left = 0;
    conn->ch.data_ended = false;

    initStringInfo(&buf);
    appendBinaryStringInfo(&buf, (char*)&len, sizeof(len)); /* patched below */
    appendBinaryStringInfo(&buf, (char*)&index, sizeof(index));
    chdb_helper_build_setup(&buf, &ctx, sql, NULL, NULL, 0);
    len = (uint32_t)(buf.len - sizeof(len));
    memcpy(buf.data, &len, sizeof(len));

    chdb_channel_send_exact(&conn->ch, buf.data, buf.len);
    pfree(buf.data);
}

/* Reads the status frame and raises the worker's error if it carries one. */
static void
read_status(chdbSearchConn* conn) {
    uint8_t status;
    uint32_t len;

    chdb_channel_recv_exact(&conn->ch, &status, sizeof(status));
    chdb_channel_recv_exact(&conn->ch, &len, sizeof(len));
    if (len > CHDB_SEARCH_CHUNK_MAX) {
        lost_worker(&conn->ch, "worker sent a bad status", 0);
    }

    char* detail = palloc(len + 1);

    chdb_channel_recv_exact(&conn->ch, detail, len);
    detail[len] = '\0';
    if (status == 0) {
        pfree(detail);
        return;
    }

    /* Worded as the helper's errors are: no trailing newline, request ID or version. */
    while (len && (detail[len - 1] == '\n' || detail[len - 1] == '\r')) {
        detail[--len] = '\0';
    }
    chdb_channel_scrub_error(detail, len);
    ereport(
        ERROR,
        errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
        errmsg("chdb_search: error executing chDB query"),
        errdetail("%s", detail),
        errcontext("query: %s", conn->query)
    );
}

void
chdb_search_exec(chdbSearchConn* conn, Oid indexoid, const char* sql) {
    send_request(conn, CHDB_CMD_EXEC, indexoid, sql);
    read_status(conn);
}

void
chdb_search_drop(chdbSearchConn* conn, Oid indexoid) {
    send_request(conn, CHDB_CMD_DROP, indexoid, "DROP");
    read_status(conn);
}

void
chdb_search_select(chdbSearchConn* conn, Oid indexoid, const char* sql) {
    send_request(conn, CHDB_CMD_SELECT, indexoid, sql);
}

void
chdb_search_insert(chdbSearchConn* conn, Oid indexoid, const char* sql) {
    send_request(conn, CHDB_CMD_INSERT, indexoid, sql);
}

chdbChannel*
chdb_search_channel(chdbSearchConn* conn) {
    return &conn->ch;
}

void
chdb_search_finish(chdbSearchConn* conn) {
    if (conn->cmd == CHDB_CMD_INSERT) {
        chdb_channel_end_write(&conn->ch);
    } else {
        /* Reader stopped early: skip what is left to get to the status. */
        char skip[8192];

        while (chdb_channel_recv(&conn->ch, skip, sizeof(skip))) {}
    }
    read_status(conn);
}
