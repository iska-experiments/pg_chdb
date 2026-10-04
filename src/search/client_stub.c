/*
 * A per-backend fake of the worker client (client.h), linked when make is
 * given CHDB_SEARCH_STUB=1. It lets the access method be built and tested
 * without a worker: requests are logged at DEBUG2 by kind (the AM logs every
 * statement it generates at DEBUG1, so the SQL is not repeated here). The
 * connection's channel reads and writes /dev/null, so selects return no
 * blocks and inserted data is dropped.
 */

#include "postgres.h"

#include <errno.h>
#include <fcntl.h>

#include "client.h"

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

/* A fresh /dev/null for each stream, as reading to its end closes it. */
static void
open_stream(chdbSearchConn* conn, const char* kind) {
    elog(DEBUG2, "chdb_search stub: %s", kind);
    chdb_channel_close_fd(&conn->ch.data);
    conn->ch.data = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (conn->ch.data < 0) {
        ereport(
            ERROR,
            errcode_for_file_access(),
            errmsg("chdb_search stub: could not open /dev/null: %m")
        );
    }
}

/* The channel closes with the memory context, so the connection is not freed. */
chdbSearchConn*
chdb_search_connect(void) {
    chdbSearchConn* conn = palloc0(sizeof(*conn));

    chdb_channel_init(&conn->ch, -1, -1);
    conn->ch.fail = stub_failed;
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
) {
    elog(DEBUG2, "chdb_search stub: exec");
}

void
chdb_search_select(
    chdbSearchConn* conn,
    Oid indexoid,
    uint64 generation,
    const char* sql
) {
    open_stream(conn, "select");
}

void
chdb_search_insert(
    chdbSearchConn* conn,
    Oid indexoid,
    uint64 generation,
    const char* sql
) {
    open_stream(conn, "insert");
}

void
chdb_search_finish(chdbSearchConn* conn) {
    elog(DEBUG2, "chdb_search stub: finish");
}

void
chdb_search_drop(chdbSearchConn* conn, Oid indexoid) {
    elog(DEBUG2, "chdb_search stub: drop");
}

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
