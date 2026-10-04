/*
 * Backend side of the chdb_search worker protocol: connecting, starting the
 * worker when it is not there, and framing requests and status replies. The
 * Native blocks in between flow through the chdbChannel of channel.h, the same
 * transport COPY uses to talk to chdb_helper. See protocol.h for the framing.
 */

#include "postgres.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "access/xact.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "client.h"
#include "frame.h"
#include "protocol.h"
#include "worker.h"

/* Milliseconds between connection attempts while the worker comes up. */
#define CHDB_SEARCH_RETRY_MS 20

struct chdbSearchConn {
    chdbChannel ch;
    chdbCmdType cmd;   /* of the request under way, so finish knows which end it is */
    Oid index;         /* of the request under way, for the no-store error */
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

void
chdb_search_client_init(void) {
    DefineCustomIntVariable(
        "chdb_search.worker_timeout",
        "Seconds to wait for the chdb_search worker to start.",
        NULL,
        &chdb_search_worker_timeout,
        30,
        1,
        3600,
        PGC_USERSET,
        GUC_UNIT_S,
        NULL,
        NULL,
        NULL
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
    /* Nonblocking, so that a full backlog is an EAGAIN to retry, not a hang. */
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
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
    conn->ch.wait_what = "timed out waiting for the worker";
    conn->ch.fail      = lost_worker;
    /*
     * The worker serves one request at a time, so a request can wait behind
     * another backend's build or OPTIMIZE; inside a commit or abort callback
     * (the store drops of drop.c) that wait cannot be cancelled, so it is
     * bounded as the connect is. The drop then warns and the sweep removes
     * the store later.
     */
    conn->ch.hold_timeout_ms = chdb_search_worker_timeout * 1000;
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
                    "See the server log, and that chdb_search_engine finds libchdb."
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

/* Sends the request frame of protocol.h. */
static void
send_request(
    chdbSearchConn* conn,
    chdbCmdType cmd,
    Oid index,
    uint64 generation,
    const char* sql
) {
    StringInfoData buf;

    conn->cmd           = cmd;
    conn->index         = index;
    conn->query         = sql;
    conn->ch.chunk_left = 0;
    conn->ch.data_ended = false;

    initStringInfo(&buf);
    chdb_search_frame_request(&buf, cmd, index, generation, sql);
    chdb_channel_send_exact(&conn->ch, buf.data, buf.len);
    pfree(buf.data);
}

/*
 * Reads the status frame and raises the worker's error if it carries one.
 * Otherwise returns the text of a debug command's answer, empty for the rest.
 */
static char*
read_status(chdbSearchConn* conn) {
    uint8_t status;
    uint32_t len;

    chdb_channel_recv_exact(&conn->ch, &status, sizeof(status));
    chdb_channel_recv_exact(&conn->ch, &len, sizeof(len));
    if (len > CHDB_CHANNEL_CHUNK_MAX) {
        lost_worker(&conn->ch, "worker sent a bad status", 0);
    }

    char* detail = palloc(len + 1);

    chdb_channel_recv_exact(&conn->ch, detail, len);
    detail[len] = '\0';
    if (status == CHDB_STATUS_OK) {
        return detail;
    }
    if (status == CHDB_STATUS_NO_STORE) {
        /* Outside a transaction (a commit callback) the name cannot be looked up. */
        char* name = IsTransactionState() ? get_rel_name(conn->index) : NULL;

        ereport(
            ERROR,
            errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            name ? errmsg("chdb index \"%s\" does not match its store", name)
                 : errmsg("chdb index %u does not match its store", conn->index),
            errdetail("%s", detail),
            errhint("REINDEX INDEX rebuilds the store.")
        );
    }
    ereport(
        ERROR,
        errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
        errmsg("chdb_search: error executing chDB query"),
        errdetail("%s", detail),
        errcontext("query: %s", conn->query)
    );

    return NULL;
}

void
chdb_search_exec(
    chdbSearchConn* conn,
    Oid indexoid,
    uint64 generation,
    const char* sql
) {
    send_request(conn, CHDB_CMD_EXEC, indexoid, generation, sql);
    read_status(conn);
}

void
chdb_search_drop(chdbSearchConn* conn, Oid indexoid) {
    send_request(conn, CHDB_CMD_DROP, indexoid, 0, "DROP");
    read_status(conn);
}

void
chdb_search_select(
    chdbSearchConn* conn,
    Oid indexoid,
    uint64 generation,
    const char* sql
) {
    send_request(conn, CHDB_CMD_SELECT, indexoid, generation, sql);
}

void
chdb_search_insert(
    chdbSearchConn* conn,
    Oid indexoid,
    uint64 generation,
    const char* sql
) {
    send_request(conn, CHDB_CMD_INSERT, indexoid, generation, sql);
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

int
chdb_search_engine_pid(chdbSearchConn* conn) {
    send_request(conn, CHDB_CMD_ENGINE_PID, 0, 0, "");

    return atoi(read_status(conn));
}

int
chdb_search_engine_kill(chdbSearchConn* conn, int signo) {
    send_request(conn, CHDB_CMD_ENGINE_KILL, 0, 0, psprintf("%d", signo));

    return atoi(read_status(conn));
}
