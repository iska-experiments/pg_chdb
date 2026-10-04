/*
 * The Native stream from chDB to Postgres: the block source a pgch_reader
 * decodes from, and the readers of a DESCRIBE and of a query's rows. The one
 * TU defining the clickhouse-c and pg-clickhouse-c implementations, both
 * halves: the encoder's header stays included for that though native_send.c
 * is what encodes.
 *
 * Values cross as Datums in both directions: a pgch_writer fed from scan slots
 * on the way out (native_send.c), a pgch_reader over the helper's output
 * feeding an insert loop on the way in (native_recv.c). Nothing passes
 * through COPY's text escaping, so arrays, decimals and timestamps keep their
 * types instead of collapsing to String.
 */

#include "postgres.h"

#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#define CHC_IMPLEMENTATION
#define PGCH_IMPLEMENTATION
#include "clickhouse.h"

#include "pg-clickhouse-decode.h"
#include "pg-clickhouse-encode.h"

#include "native.h"

/* ---- chDB to Postgres ------------------------------------------------ */

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

/*
 * Read one source column from each DESCRIBE row
 * describe_compact_output limits result to String name and type columns
 */
List*
chdb_native_describe(chdbChannel* helper) {
    /* Keep reader buffers temporary and allocate returned columns in caller context */
    MemoryContext streamcxt = AllocSetContextCreate(
        CurrentMemoryContext, "chdb describe", ALLOCSET_SMALL_SIZES
    );
    MemoryContext oldcxt  = MemoryContextSwitchTo(streamcxt);
    pgch_block_source src = chdb_native_source(helper);
    List* columns         = NIL;
    pgch_reader reader;

    pgch_reader_init(&reader, &src);
    if (reader.error) {
        chdb_native_reader_error(reader.error);
    }
    if (pgch_reader_columns(&reader) != 2) {
        ereport(
            ERROR,
            errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
            errmsg(
                "chdb: DESCRIBE returned %zu columns, expected 2",
                pgch_reader_columns(&reader)
            )
        );
    }

    while (pgch_reader_next(&reader)) {
        Datum values[2];
        bool nulls[2];

        pgch_reader_fill(&reader, NULL, values, nulls);
        if (nulls[0] || nulls[1]) {
            ereport(
                ERROR,
                errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
                errmsg("chdb: DESCRIBE produced a column with no name or type")
            );
        }

        MemoryContextSwitchTo(oldcxt);
        chdbDescribedColumn* col = palloc(sizeof(*col));

        col->name = TextDatumGetCString(values[0]);
        col->type = TextDatumGetCString(values[1]);
        columns   = lappend(columns, col);
        MemoryContextSwitchTo(streamcxt);
    }
    if (reader.error) {
        chdb_native_reader_error(reader.error);
    }

    MemoryContextSwitchTo(oldcxt);
    MemoryContextDelete(streamcxt);

    return columns;
}
