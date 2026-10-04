/*
 * A per-backend fake of the worker client (client.h), linked when make is
 * given CHDB_SEARCH_STUB=1. It lets the access method be built and tested
 * without a worker: requests are logged at DEBUG1 by kind (the AM itself logs
 * every generated statement, so the SQL is not repeated here), selects return
 * no blocks, inserted data is dropped.
 */

#include "postgres.h"

#include "client.h"

struct chdbSearchConn {
    bool open;
};

chdbSearchConn*
chdb_search_connect(void) {
    chdbSearchConn* conn = palloc0(sizeof(*conn));

    conn->open = true;
    return conn;
}

void
chdb_search_close(chdbSearchConn* conn) {
    if (conn) {
        conn->open = false;
        pfree(conn);
    }
}

void
chdb_search_exec(chdbSearchConn* conn, Oid indexoid, const char* sql) {
    elog(DEBUG1, "chdb_search stub: exec");
}

void
chdb_search_select(chdbSearchConn* conn, Oid indexoid, const char* sql) {
    elog(DEBUG1, "chdb_search stub: select");
}

size_t
chdb_search_recv(chdbSearchConn* conn, void* buf, size_t len) {
    return 0;
}

void
chdb_search_insert(chdbSearchConn* conn, Oid indexoid, const char* sql) {
    elog(DEBUG1, "chdb_search stub: insert");
}

void
chdb_search_send(chdbSearchConn* conn, const void* buf, size_t len) {
    elog(DEBUG1, "chdb_search stub: send %s", len ? "block" : "nothing");
}

void
chdb_search_finish(chdbSearchConn* conn) {
    elog(DEBUG1, "chdb_search stub: finish");
}

void
chdb_search_drop(chdbSearchConn* conn, Oid indexoid) {
    elog(DEBUG1, "chdb_search stub: drop");
}
