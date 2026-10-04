/*
 * A per-backend fake of the worker client (client.h), linked when make is
 * given CHDB_SEARCH_STUB=1, so the access method builds and tests without
 * a worker: statements are accepted, inserted data is dropped, and a select
 * answers with the rows these GUCs describe.
 *
 *   chdb_search_stub.ctids        the packed ctids ((block << 16) | offset,
 *                                 so (0,1) is 1 and (1,1) is 65537) a select
 *                                 returns, comma-separated, one row each,
 *                                 with one Float64 column holding the ctid
 *                                 per ` AS _distance` in the statement and
 *                                 one Float32 per ` AS _score`; 'garbage'
 *                                 returns bytes that are not a Native block;
 *                                 empty, the default, returns no block at
 *                                 all. A count(), as the aggregate scan sends
 *                                 for count(*), is their number.
 *   chdb_search_stub.fail         reading the answer fails, as a lost worker
 *                                 would
 *   chdb_search_stub.meta         what the store says of the index's
 *                                 generation when the fail-safe check
 *                                 (meta.c) asks: empty, the default, agrees
 *                                 with the metapage; 'none' has neither
 *                                 table nor flush for it; a number is the
 *                                 WAL position of its last flush
 *   chdb_search_stub.tokens       what tokens() makes of any needle,
 *                                 comma-separated
 *   chdb_search_stub.frequencies  `token:n` pairs, comma-separated: the
 *                                 count() of the rows with a token, as the
 *                                 score asks it (score.c), which is the one
 *                                 count() not answered from the ctids; zero
 *                                 for a token not listed
 *
 * The answers themselves are encoded in stub_answers.c. The AM logs every
 * statement it generates at DEBUG1; nothing is logged here.
 */

#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "utils/guc.h"
#include "utils/memutils.h"

#include "client.h"
#include "search.h"
#include "stub.h"

char* chdb_search_stub_ctids       = NULL;
char* chdb_search_stub_meta        = NULL;
char* chdb_search_stub_tokens      = NULL;
char* chdb_search_stub_frequencies = NULL;
static bool stub_fail              = false;

struct chdbSearchConn {
    chdbChannel ch;
};

static void
stub_failed(chdbChannel* ch, const char* what, int errnum) {
    errno = errnum;
    ereport(
        ERROR,
        errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
        errmsg("chdb_search stub: %s: %m", what)
    );
}

void
chdb_search_client_init(void) {
    DefineCustomStringVariable(
        "chdb_search_stub.ctids",
        "Packed ctids the stub worker client's selects return, comma-separated.",
        "Empty returns no rows; 'garbage' returns bytes that are not a Native block.",
        &chdb_search_stub_ctids,
        "",
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    DefineCustomBoolVariable(
        "chdb_search_stub.fail",
        "Whether reading the stub worker client's answer fails, as a lost worker "
        "would.",
        NULL,
        &stub_fail,
        false,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    DefineCustomStringVariable(
        "chdb_search_stub.meta",
        "What the stub worker client's store says of the index's generation.",
        "Empty agrees with the metapage; 'none' has no table and no flush for it; a "
        "number is the WAL position of its last flush.",
        &chdb_search_stub_meta,
        "",
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    DefineCustomStringVariable(
        "chdb_search_stub.tokens",
        "What the stub worker client's tokens() makes of any needle, comma-separated.",
        NULL,
        &chdb_search_stub_tokens,
        "",
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    DefineCustomStringVariable(
        "chdb_search_stub.frequencies",
        "The rows the stub worker client counts with a token, as token:n pairs.",
        "Comma-separated; a token not listed counts zero.",
        &chdb_search_stub_frequencies,
        "",
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    MarkGUCPrefixReserved("chdb_search_stub");
}

/* Replaces the data descriptor, as reading a stream to its end closes it. */
static void
set_data_fd(chdbSearchConn* conn, int fd, const char* what) {
    if (fd < 0) {
        ereport(
            ERROR,
            errcode_for_file_access(),
            errmsg("chdb_search stub: could not open %s: %m", what)
        );
    }
    chdb_channel_close_fd(&conn->ch.data);
    conn->ch.data = fd;
}

/*
 * Makes `len` bytes the stream's answer, through a pipe the channel reads to
 * its end. The pipe must hold the whole answer, as nothing reads it yet.
 */
static void
answer(chdbSearchConn* conn, const void* data, size_t len) {
    int fds[2];

    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
        set_data_fd(conn, -1, "a pipe");
    }
    for (const char* at = data; len;) {
        ssize_t put = write(fds[1], at, len);

        if (put < 0 && errno == EINTR) {
            continue;
        }
        if (put < 0) {
            ereport(
                ERROR,
                errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                errmsg("chdb_search stub: the answer does not fit in a pipe: %m")
            );
        }
        at += put;
        len -= put;
    }
    close(fds[1]);
    set_data_fd(conn, fds[0], "a pipe");
}

/* The channel closes with the memory context, so the connection is not freed. */
chdbSearchConn*
chdb_search_connect(void) {
    chdbSearchConn* conn = palloc0(sizeof(*conn));

    chdb_channel_init(&conn->ch, -1, -1);
    conn->ch.recv_what = "error receiving from the worker";
    conn->ch.send_what = "error sending to the worker";
    conn->ch.fail      = stub_failed;
    chdb_channel_own(&conn->ch);
    return conn;
}

void
chdb_search_close(chdbSearchConn* conn) {
    if (conn) {
        chdb_channel_close(&conn->ch);
    }
}

void
chdb_search_exec(
    chdbSearchConn* conn,
    Oid indexoid,
    uint64 generation,
    const char* sql
) {}

void
chdb_search_select(
    chdbSearchConn* conn,
    Oid indexoid,
    uint64 generation,
    const char* sql
) {
    void* block;
    size_t len;

    if (stub_fail) {
        /* Write-only, so the first read fails as a broken connection does. */
        set_data_fd(conn, open("/dev/null", O_WRONLY | O_CLOEXEC), "/dev/null");
    } else if (strcmp(chdb_search_stub_ctids, "garbage") == 0) {
        answer(conn, "not a Native block", 18);
    } else {
        len = chdb_stub_answer(sql, indexoid, &block);
        answer(conn, block, len);
        pfree(block);
    }
}

/* Inserted data goes to /dev/null. */
void
chdb_search_insert(
    chdbSearchConn* conn,
    Oid indexoid,
    uint64 generation,
    const char* sql
) {
    set_data_fd(conn, open("/dev/null", O_RDWR | O_CLOEXEC), "/dev/null");
}

void
chdb_search_finish(chdbSearchConn* conn) {}

void
chdb_search_drop(chdbSearchConn* conn, Oid indexoid) {}

int
chdb_search_engine_pid(chdbSearchConn* conn) {
    return 0;
}

int
chdb_search_engine_kill(chdbSearchConn* conn, int signo) {
    ereport(
        ERROR,
        errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
        errmsg("chdb_search stub: there is no engine to signal")
    );
}

chdbChannel*
chdb_search_channel(chdbSearchConn* conn) {
    return &conn->ch;
}
