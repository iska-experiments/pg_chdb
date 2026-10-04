/*
 * The one translation unit of chdb_search defining the clickhouse-c and
 * pg-clickhouse-c implementations, and the Native block source over a worker
 * connection.
 *
 * src/native.c does the same over a chdbHelper pipe, but its source and sink
 * are static and tied to that type, and it cannot be linked here (it needs the
 * helper's symbols and heap-insert machinery). The reading side is the same
 * 40 lines over chdb_search_recv.
 */

#include "postgres.h"

#include "miscadmin.h"
#include "utils/memutils.h"

#define CHC_IMPLEMENTATION
#define PGCH_IMPLEMENTATION
#include "clickhouse.h"

#include "pg-clickhouse-decode.h"
#include "pg-clickhouse-encode.h"

#include "search.h"

#define SOURCE_BYTES (256 * 1024)

typedef struct nativeSource {
    chc_io io;
    chc_in* in;
    MemoryContext cxt; /* decoded blocks outlive the rows read from them */
    chdbSearchConn* conn;
    char* error;
} nativeSource;

/* Worker failures raise from chdb_search_recv, so this always succeeds. */
static int
source_read(
    void* ud,
    void* buf,
    size_t len,
    size_t* got,
    chc_err* err pg_attribute_unused()
) {
    nativeSource* s = ud;

    *got = chdb_search_recv(s->conn, buf, len);
    return CHC_OK;
}

static int
source_cancelled(void* ud pg_attribute_unused()) {
    CHECK_FOR_INTERRUPTS();
    return 0;
}

static const chc_block*
source_next(void* ud) {
    nativeSource* s = ud;
    chc_block* block;
    chc_err err = {};

    if (s->error) {
        return NULL;
    }

    MemoryContext old = MemoryContextSwitchTo(s->cxt);

    /* A NULL block without an error marks the end of the stream. */
    if (chc_block_read(s->in, &pgch_alloc, &pgch_block_opts_local, &block, &err) !=
        CHC_OK) {
        s->error = MemoryContextStrdup(
            s->cxt, err.msg[0] ? err.msg : "chDB block could not be read"
        );
        block = NULL;
    }
    MemoryContextSwitchTo(old);
    return block;
}

static const char*
source_error(void* ud) {
    return ((nativeSource*)ud)->error;
}

pgch_block_source
chdb_search_block_source(chdbSearchConn* conn, MemoryContext cxt) {
    MemoryContext old = MemoryContextSwitchTo(cxt);
    nativeSource* s   = palloc0(sizeof(*s));
    chc_err err       = {};

    s->conn = conn;
    s->cxt  = cxt;
    s->io = (chc_io){ .ud = s, .read = source_read, .check_cancel = source_cancelled };
    s->in = pgch_in_alloc();
    if (chc_in_init(s->in, &s->io, &pgch_alloc, SOURCE_BYTES, &err) != CHC_OK) {
        pgch_raise(&err, ERRCODE_EXTERNAL_ROUTINE_EXCEPTION, "reader init: ", NULL);
    }
    MemoryContextSwitchTo(old);

    return (pgch_block_source){ .ud         = s,
                                .next_block = source_next,
                                .error      = source_error };
}
