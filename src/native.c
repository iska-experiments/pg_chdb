/*
 * The Native stream between Postgres and chDB: the block source a pgch_reader
 * decodes from, and the one TU defining the clickhouse-c and pg-clickhouse-c
 * implementations, both halves, so the encoder's header is included here
 * though nothing below encodes.
 *
 * Values cross as Datums in both directions: a pgch_writer fed from scan slots
 * on the way out (native_send.c), a pgch_reader over this source on the way
 * in, feeding an insert loop (native_recv.c), a tuplestore (native_select.c)
 * or a column list (native_describe.c). Nothing passes through COPY's text
 * escaping, so arrays, decimals and timestamps keep their types instead of
 * collapsing to String.
 */

#include "postgres.h"

#include "miscadmin.h"
#include "utils/memutils.h"

#define CHC_IMPLEMENTATION
#define PGCH_IMPLEMENTATION
#include "clickhouse.h"

#include "pg-clickhouse-decode.h"
#include "pg-clickhouse-encode.h"

#include "native.h"

/*
 * Buffer small reads from helper. Read large chunks directly into destination,
 * so only block metadata usually passes through this buffer.
 */
#define CHDB_NATIVE_SOURCE_BYTES (256 * 1024)

/* Reads Native output from helper one block at a time. */
typedef struct nativeSource {
    chc_io io;
    chc_in* in;
    MemoryContext cxt; /* decoded blocks outlive rows read from them */
    chdbChannel* helper;
    char* error;
} nativeSource;

/* Helper failures are reported directly, so callback always returns success. */
static int
source_read(
    void* ud,
    void* buf,
    size_t len,
    size_t* got,
    chc_err* err pg_attribute_unused()
) {
    nativeSource* s = ud;

    *got = chdb_channel_recv(s->helper, buf, len);

    return CHC_OK;
}

/* Checks for interrupts before decoder refills input buffer. */
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

    MemoryContext oldcxt = MemoryContextSwitchTo(s->cxt);

    /* NULL block without an error marks end of stream. */
    if (chc_block_read(s->in, &pgch_alloc, &pgch_block_opts_local, &block, &err) !=
        CHC_OK) {
        s->error = MemoryContextStrdup(
            s->cxt, err.msg[0] ? err.msg : "chDB block could not be read"
        );
        block = NULL;
    }
    MemoryContextSwitchTo(oldcxt);

    return block;
}

static const char*
source_error(void* ud) {
    return ((nativeSource*)ud)->error;
}

pgch_block_source
chdb_native_source(chdbChannel* helper) {
    nativeSource* s = palloc0(sizeof(*s));
    chc_err err     = {};

    s->helper = helper;
    s->cxt    = CurrentMemoryContext;
    s->io = (chc_io){ .ud = s, .read = source_read, .check_cancel = source_cancelled };
    s->in = pgch_in_alloc();
    if (chc_in_init(s->in, &s->io, &pgch_alloc, CHDB_NATIVE_SOURCE_BYTES, &err) !=
        CHC_OK) {
        pgch_raise(&err, ERRCODE_EXTERNAL_ROUTINE_EXCEPTION, "reader init: ", NULL);
    }

    return (pgch_block_source){ .ud         = s,
                                .next_block = source_next,
                                .error      = source_error };
}

/* Reader errors carry the chDB message; the query text is the caller's. */
pg_noreturn void
chdb_native_reader_error(const char* error) {
    ereport(
        ERROR,
        errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
        errmsg("chdb: error fetching chDB query result"),
        errdetail("%s", error)
    );
}
