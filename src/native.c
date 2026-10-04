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
#include "utils/tuplestore.h"

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

/*
 * Execute a query against a temporary chDB database and return its rows,
 * mapping chDB values to the Postgres types named in the caller's column
 * definition list and streaming the results back to the client.
 */
Datum
chdb_select_receive(
    char* query,
    ReturnSetInfo* rsinfo,
    TupleDesc tupdesc,
    chdbChannel* helper
) {
    /*
     * Build result info in per-query context so it outlives this call.
     */
    MemoryContext query_ctx   = rsinfo->econtext->ecxt_per_query_memory;
    MemoryContext old_ctx     = MemoryContextSwitchTo(query_ctx);
    tupdesc                   = CreateTupleDescCopy(tupdesc);
    Tuplestorestate* tupstore = tuplestore_begin_heap(
        rsinfo->allowedModes & SFRM_Materialize_Random, false, work_mem
    );

    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult  = tupstore;
    rsinfo->setDesc    = tupdesc;

    MemoryContextSwitchTo(old_ctx);

    /* Set up row destination and the query reader. */
    Datum* values = palloc(tupdesc->natts * sizeof(Datum));
    bool* nulls   = palloc0(tupdesc->natts * sizeof(bool));

    pgch_block_source src = chdb_native_source(helper);
    pgch_reader reader;
    pgch_reader_init(&reader, &src);
    if (reader.error) {
        chdb_native_reader_error(reader.error);
    }

    /* Per-row values are copied into tuplestore; reset between rows. */
    MemoryContext row_cxt = AllocSetContextCreate(
        CurrentMemoryContext, "chdb_query row", ALLOCSET_DEFAULT_SIZES
    );

    /*
     * Fetch the columns from the chDB query. Use row context because it
     * fetches the first block to get the columns.
     */
    MemoryContextSwitchTo(row_cxt);
    if (pgch_reader_columns(&reader) == 0) {
        /* Nothing streamed at all, so there is no schema to check. */
        MemoryContextSwitchTo(old_ctx);
        MemoryContextDelete(row_cxt);
        return (Datum)0;
    }
    if (pgch_reader_columns(&reader) != tupdesc->natts) {
        ereport(
            ERROR,
            errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
            errmsg(
                "chdb: chDB returned %zu columns, expected %d",
                pgch_reader_columns(&reader),
                tupdesc->natts
            )
        );
    }

    /*
     * Populate the conversion state per column, from the column type, not the
     * value. Configure the Postgres destination column attnum per chDB column
     * so tuplestore_putvalues() below knows where to put things.
     */
    MemoryContextSwitchTo(query_ctx);
    void** states  = palloc0(tupdesc->natts * sizeof(void*));
    int* attr_nums = palloc(tupdesc->natts * sizeof(int));
    for (size_t i = 0; i < tupdesc->natts; i++) {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        attr_nums[i]           = attr->attnum - 1;
        states[i] =
            pgch_reader_convert_init(&reader, i, attr->atttypid, attr->atttypmod);
    }

    /* Fetch the data from chDB. */
    for (;;) {
        MemoryContextSwitchTo(row_cxt);
        if (!pgch_reader_next(&reader)) {
            /* No more rows to process. */
            MemoryContextSwitchTo(old_ctx);
            break;
        }

        /*
         * Use states to convert values from reader and store in values &
         * nulls in positions defined by attr_nums.
         */
        pgch_reader_fill_map(&reader, states, attr_nums, values, nulls);

        /* Send the resulting Datums in the tuple store for Postgres to process. */
        tuplestore_putvalues(tupstore, tupdesc, values, nulls);

        MemoryContextReset(row_cxt);
        CHECK_FOR_INTERRUPTS();
    }

    /* Clean up and return. */
    MemoryContextDelete(row_cxt);
    return (Datum)0;
}
