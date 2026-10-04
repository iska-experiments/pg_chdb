/*
 * A per-backend fake of the worker client (client.h), linked when make is
 * given CHDB_SEARCH_STUB=1. It lets the access method be built and tested
 * without a worker: requests are logged at DEBUG2 by kind (the AM logs every
 * statement it generates at DEBUG1, so the SQL is not repeated here), selects
 * return no blocks, inserted data is dropped. Streams through
 * chdb_search_helper behave the same way.
 */

#include "postgres.h"

#include "../helper.h"
#include "client.h"

struct chdbSearchConn {
    bool open;
};

struct chdbHelper {
    chdbSearchConn* conn;
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
    elog(DEBUG2, "chdb_search stub: exec");
}

void
chdb_search_select(chdbSearchConn* conn, Oid indexoid, const char* sql) {
    elog(DEBUG2, "chdb_search stub: select");
}

size_t
chdb_search_recv(chdbSearchConn* conn, void* buf, size_t len) {
    return 0;
}

void
chdb_search_insert(chdbSearchConn* conn, Oid indexoid, const char* sql) {
    elog(DEBUG2, "chdb_search stub: insert");
}

void
chdb_search_send(chdbSearchConn* conn, const void* buf, size_t len) {
    elog(DEBUG2, "chdb_search stub: send %s", len ? "block" : "nothing");
}

void
chdb_search_finish(chdbSearchConn* conn) {
    elog(DEBUG2, "chdb_search stub: finish");
}

void
chdb_search_drop(chdbSearchConn* conn, Oid indexoid) {
    elog(DEBUG2, "chdb_search stub: drop");
}

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
