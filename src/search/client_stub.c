/*
 * A per-backend fake of the worker client (client.h), linked when make is
 * given CHDB_SEARCH_STUB=1. It lets the access method be built and tested
 * without a worker: statements are accepted, inserted data is dropped, and
 * a select answers with the rows two GUCs describe, so a test can hand a
 * scan the ctids it should fetch and VACUUM the rows it should judge.
 *
 *   chdb_search_stub.ctids  the packed ctids ((block << 16) | offset, so
 *                           (0,1) is 1 and (1,1) is 65537) a select returns,
 *                           comma-separated, one row each, with one Float64
 *                           column holding the ctid per ` AS _distance` in
 *                           the statement; 'garbage' returns bytes that are
 *                           not a Native block; empty, the default, returns
 *                           no block at all
 *   chdb_search_stub.fail   reading the answer fails, as a lost worker would
 *
 * The AM logs every statement it generates at DEBUG1, so nothing is logged
 * here.
 */

#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "catalog/pg_type_d.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "pg-clickhouse-encode.h"
#include "pg-clickhouse.h"

#include "client.h"

static char* stub_ctids = NULL;
static bool stub_fail   = false;

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
        &stub_ctids,
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

/*
 * One Native block of the GUC's ctids, with `ndist` distance columns, built
 * with the encoder the row writer uses. Returns the length, zero for no rows.
 */
static size_t
encode_ctids(const char* ctids, int ndist, void** out) {
    MemoryContext cxt = AllocSetContextCreate(
        CurrentMemoryContext, "chdb_search stub block", ALLOCSET_SMALL_SIZES
    );
    MemoryContext old = MemoryContextSwitchTo(cxt);
    StringInfoData structure;
    chc_type* type;
    chc_err err  = {};
    pgch_buf buf = {};

    initStringInfo(&structure);
    appendStringInfoString(&structure, "Tuple(ctid UInt64");
    for (int i = 0; i < ndist; i++) {
        appendStringInfo(&structure, ", _distance%d Float64", i);
    }
    appendStringInfoChar(&structure, ')');
    if (chc_type_parse(structure.data, structure.len, &pgch_alloc, &type, &err) !=
        CHC_OK) {
        pgch_raise(&err, ERRCODE_INTERNAL_ERROR, "chdb_search stub: ", NULL);
    }

    size_t ncols   = chc_type_n_children(type);
    pgch_col* cols = palloc0(ncols * sizeof(pgch_col));

    for (size_t i = 0; i < ncols; i++) {
        cols[i].name = chc_type_tuple_field_name(type, i, &cols[i].name_len);
        cols[i].type = chc_type_child(type, i);
    }

    pgch_writer* w = pgch_writer_new(cxt, cols, ncols);

    for (const char* p = ctids; *p;) {
        char* end;
        uint64 ctid = strtoull(p, &end, 10);

        if (end == p || (*end != ',' && *end != '\0')) {
            ereport(
                ERROR,
                errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("chdb_search stub: invalid ctid list \"%s\"", ctids)
            );
        }
        pgch_append_datum(w, 0, Int64GetDatum((int64)ctid), INT8OID, false);
        for (int i = 0; i < ndist; i++) {
            pgch_append_datum(w, 1 + i, Float8GetDatum((double)ctid), FLOAT8OID, false);
        }
        p = *end == ',' ? end + 1 : end;
    }
    if (pgch_writer_rows(w)) {
        pgch_writer_flush(w, &buf, NULL);
    }
    MemoryContextSwitchTo(old);

    *out = palloc(buf.len + 1);
    memcpy(*out, buf.data, buf.len);
    MemoryContextDelete(cxt);
    return buf.len;
}

/* Counts the distance columns a scan's statement selects. */
static int
count_distances(const char* sql) {
    int n = 0;

    for (const char* p = sql; (p = strstr(p, " AS _distance")); p += 1) {
        n++;
    }
    return n;
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
    } else if (strcmp(stub_ctids, "garbage") == 0) {
        answer(conn, "not a Native block", 18);
    } else {
        len = encode_ctids(stub_ctids, count_distances(sql), &block);
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
