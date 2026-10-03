/*
 * Backend side of the chdb_search worker protocol: connecting, starting the
 * worker when it is not there, and trading framed requests and Native blocks.
 * See protocol.h for the framing.
 *
 * Like helper.c, every wait is interruptible and the connection is closed by
 * a memory context callback, so an error anywhere cannot leak the descriptor.
 */

#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "../helper.h"
#include "client.h"
#include "protocol.h"
#include "worker.h"

/* Milliseconds between connection attempts while the worker comes up. */
#define CHDB_SEARCH_RETRY_MS 20

typedef enum connState {
    CONN_IDLE,
    CONN_SELECTING, /* reading the worker's data chunks */
    CONN_INSERTING, /* sending data chunks */
} connState;

struct chdbSearchConn {
    MemoryContextCallback cleanup;
    int fd; /* -1 once closed */
    connState state;
    uint32_t chunk_left; /* bytes of the current inbound chunk not yet read */
    bool data_ended;     /* the zero chunk has been read */
    const char* query;   /* for error context */
};

/*
 * native.c talks to a chdbHelper. Here that is the connection, wrapped so the
 * opaque type stays distinct and native.c needs no changes.
 */
struct chdbHelper {
    chdbSearchConn* conn;
};

static void
close_conn(chdbSearchConn* conn) {
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
}

/* Memory context callback, closes the socket. */
static void
close_callback(void* arg) {
    close_conn(arg);
}

/* Sleeps until `fd` is ready, letting a cancel or a shutdown through. */
static void
wait_fd(int fd, uint32 event) {
    WaitLatchOrSocket(
        MyLatch, event | WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, fd, -1, PG_WAIT_EXTENSION
    );
    ResetLatch(MyLatch);
    CHECK_FOR_INTERRUPTS();
}

/* Raises for a connection that broke; the stream is lost so close it. */
static void
lost_worker(chdbSearchConn* conn, const char* what) {
    int saved = errno;

    close_conn(conn);
    errno = saved;
    ereport(
        ERROR,
        errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
        errmsg("chdb_search: %s", what),
        errdetail(
            "The connection to the chdb_search worker was lost%s%s.",
            saved ? ": " : "",
            saved ? strerror(saved) : ""
        ),
        errcontext("query: %s", conn->query ? conn->query : "")
    );
}

static void
send_all(chdbSearchConn* conn, const void* buf, size_t len) {
    const char* at = buf;

    while (len) {
        CHECK_FOR_INTERRUPTS();
        if (conn->fd < 0) {
            lost_worker(conn, "error sending to the worker");
        }

        ssize_t put = send(conn->fd, at, len, MSG_NOSIGNAL);
        if (put > 0) {
            at += put;
            len -= (size_t)put;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            wait_fd(conn->fd, WL_SOCKET_WRITEABLE);
        } else if (errno != EINTR) {
            lost_worker(conn, "error sending to the worker");
        }
    }
}

/* Reads up to `len` bytes, at least one, raising at end of stream. */
static size_t
recv_some(chdbSearchConn* conn, void* buf, size_t len) {
    for (;;) {
        CHECK_FOR_INTERRUPTS();
        if (conn->fd < 0) {
            lost_worker(conn, "error receiving from the worker");
        }

        ssize_t got = recv(conn->fd, buf, len, 0);
        if (got > 0) {
            return (size_t)got;
        }
        if (got == 0) {
            errno = 0;
            lost_worker(conn, "error receiving from the worker");
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            wait_fd(conn->fd, WL_SOCKET_READABLE);
        } else if (errno != EINTR) {
            lost_worker(conn, "error receiving from the worker");
        }
    }
}

static void
recv_all(chdbSearchConn* conn, void* buf, size_t len) {
    char* at = buf;

    while (len) {
        size_t got = recv_some(conn, at, len);

        at += got;
        len -= got;
    }
}

/* ---- connecting ---------------------------------------------------------- */

/* A connected nonblocking socket, or -1 with errno saying why not. */
static int
try_connect(void) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int fd;

    /* Relative to the data directory, which backends run in. */
    snprintf(
        addr.sun_path, sizeof(addr.sun_path), CHDB_SEARCH_SOCKET_FMT, MyDatabaseId
    );
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        int saved = errno;

        close(fd);
        errno = saved;
        return -1;
    }
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) {
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

    conn->fd = -1;

    /* Registered first, so the descriptor has an owner from the start. */
    conn->cleanup.func = close_callback;
    conn->cleanup.arg  = conn;
    MemoryContextRegisterResetCallback(CurrentMemoryContext, &conn->cleanup);

    for (;;) {
        CHECK_FOR_INTERRUPTS();
        conn->fd = try_connect();
        if (conn->fd >= 0) {
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
                errhint("See the server log, and chdb_search.libchdb_path.")
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
        close_conn(conn);
    }
}

/* ---- requests ------------------------------------------------------------ */

static void
append_string(StringInfo buf, const char* str) {
    uint32_t len = (uint32_t)strlen(str);

    appendBinaryStringInfo(buf, (char*)&len, sizeof(len));
    appendBinaryStringInfo(buf, str, len);
}

/* Sends the request frame of protocol.h. */
static void
send_request(chdbSearchConn* conn, chdbCmdType cmd, Oid index, const char* sql) {
    StringInfoData buf;
    uint16_t memory  = (uint16_t)chdb_max_memory;
    uint16_t threads = (uint16_t)chdb_max_threads;
    uint16_t parsers = (uint16_t)chdb_max_parsers;
    uint16_t nparams = 0;

    /* A request begun before the last one finished would corrupt the framing. */
    if (conn->fd < 0 || conn->state != CONN_IDLE) {
        close_conn(conn);
        ereport(
            ERROR,
            errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            errmsg("chdb_search: connection is closed or has a request unfinished")
        );
    }
    conn->query      = sql;
    conn->chunk_left = 0;
    conn->data_ended = false;

    initStringInfo(&buf);
    appendBinaryStringInfo(&buf, (char*)&cmd, sizeof(cmd));
    appendBinaryStringInfo(&buf, (char*)&index, sizeof(index));
    appendBinaryStringInfo(&buf, (char*)&memory, sizeof(memory));
    appendBinaryStringInfo(&buf, (char*)&threads, sizeof(threads));
    appendBinaryStringInfo(&buf, (char*)&parsers, sizeof(parsers));
    append_string(&buf, sql);
    appendBinaryStringInfo(&buf, (char*)&nparams, sizeof(nparams));

    if (buf.len > CHDB_SETUP_MAX) {
        ereport(
            ERROR,
            errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
            errmsg("chdb_search: query is too large to send to the worker")
        );
    }
    send_all(conn, buf.data, buf.len);
    pfree(buf.data);
}

/* Reads the status frame and raises the worker's error if it carries one. */
static void
read_status(chdbSearchConn* conn) {
    uint8_t status;
    uint32_t len;

    conn->state = CONN_IDLE;
    recv_all(conn, &status, sizeof(status));
    recv_all(conn, &len, sizeof(len));
    if (len > CHDB_SEARCH_CHUNK_MAX) {
        errno = 0;
        lost_worker(conn, "worker sent a bad status");
    }

    if (status == 0) {
        if (len) {
            char* skip = palloc(len);

            recv_all(conn, skip, len);
            pfree(skip);
        }
        return;
    }

    char* detail = palloc(len + 1);

    recv_all(conn, detail, len);
    detail[len] = '\0';
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
    conn->state = CONN_SELECTING;
}

/* Reads chunk headers until data or the end of the stream. False at the end. */
static bool
next_chunk(chdbSearchConn* conn) {
    while (conn->chunk_left == 0) {
        if (conn->data_ended) {
            return false;
        }
        recv_all(conn, &conn->chunk_left, sizeof(conn->chunk_left));
        if (conn->chunk_left > CHDB_SEARCH_CHUNK_MAX) {
            errno = 0;
            lost_worker(conn, "worker sent a bad chunk");
        }
        if (conn->chunk_left == 0) {
            conn->data_ended = true;
            read_status(conn);

            return false;
        }
    }

    return true;
}

size_t
chdb_search_recv(chdbSearchConn* conn, void* buf, size_t len) {
    if (conn->state != CONN_SELECTING || !next_chunk(conn)) {
        return 0;
    }

    size_t got = recv_some(conn, buf, Min(len, conn->chunk_left));

    conn->chunk_left -= (uint32_t)got;

    return got;
}

void
chdb_search_insert(chdbSearchConn* conn, Oid indexoid, const char* sql) {
    send_request(conn, CHDB_CMD_INSERT, indexoid, sql);
    conn->state = CONN_INSERTING;
}

void
chdb_search_send(chdbSearchConn* conn, const void* buf, size_t len) {
    const char* at = buf;

    if (conn->state != CONN_INSERTING) {
        ereport(
            ERROR,
            errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            errmsg("chdb_search: no insert in progress")
        );
    }
    while (len) {
        uint32_t n = (uint32_t)Min(len, CHDB_SEARCH_CHUNK_MAX);

        send_all(conn, &n, sizeof(n));
        send_all(conn, at, n);
        at += n;
        len -= n;
    }
}

void
chdb_search_finish(chdbSearchConn* conn) {
    if (conn->state == CONN_INSERTING) {
        uint32_t zero = 0;

        send_all(conn, &zero, sizeof(zero));
        read_status(conn);
    } else if (conn->state == CONN_SELECTING) {
        /* Reader stopped early: skip what is left to get to the status. */
        char skip[8192];

        while (chdb_search_recv(conn, skip, sizeof(skip))) {}
    }
}

/* ---- native.c's view of the connection ---------------------------------- */

chdbHelper*
chdb_search_helper(chdbSearchConn* conn) {
    chdbHelper* helper = palloc(sizeof(*helper));

    helper->conn = conn;

    return helper;
}

size_t
chdb_helper_recv(chdbHelper* helper, void* buf, size_t len) {
    return chdb_search_recv(helper->conn, buf, len);
}

void
chdb_helper_write(chdbHelper* helper, const void* p, size_t len) {
    chdb_search_send(helper->conn, p, len);
}
