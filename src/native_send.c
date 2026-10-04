/*
 * COPY from Postgres to chDB: a relation scanned into a chDB child as Native
 * blocks. A pgch_writer is fed from scan slots, so values cross as Datums and
 * nothing passes through COPY's text escaping; arrays, decimals and timestamps
 * keep their types instead of collapsing to String. The block source for the
 * other direction is native.c's.
 */

#include "postgres.h"

#include <string.h>

#include "access/tableam.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "clickhouse.h"
#include "pg-clickhouse-encode.h"

#include "native.h"
#include "native_writer.h"

/*
 * Bytes to accumulate before cutting a block. ClickHouse coalesces small
 * blocks within one insert via min_insert_block_size_rows / _bytes, so the cut
 * only decides how much of the scan sits in memory.
 */
#define CHDB_NATIVE_BLOCK_BYTES (8 * 1024 * 1024)

/*
 * Buffer small writes before sending them to helper. Send writes at least as
 * large as direct-write limit without copying them into buffer first.
 */
#define CHDB_NATIVE_SINK_BYTES (256 * 1024)
#define CHDB_NATIVE_SINK_DIRECT (16 * 1024)

/* COPY's writer: an append per attribute, so the counts must agree. */
static pgch_writer*
writer_for(const char* structure, int nattrs) {
    size_t ncols;
    pgch_writer* w = chdb_writer_for(CurrentMemoryContext, structure, &ncols);

    if (ncols != (size_t)nattrs) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("chdb: structure declares %zu columns, copy has %d", ncols, nattrs)
        );
    }

    return w;
}

/* Buffers small writes before sending them to helper. */
typedef struct nativeSink {
    chc_io io;
    chdbChannel* helper;
    size_t len;
    uint8_t buf[CHDB_NATIVE_SINK_BYTES];
} nativeSink;

static void
sink_flush(nativeSink* s) {
    if (s->len) {
        chdb_channel_write(s->helper, s->buf, s->len);
        s->len = 0;
    }
}

static int
sink_write(void* ud, const void* p, size_t len, chc_err* err pg_attribute_unused()) {
    nativeSink* s = ud;

    if (len >= CHDB_NATIVE_SINK_DIRECT) {
        sink_flush(s); /* send buffered bytes first */
        chdb_channel_write(s->helper, p, len);

        return CHC_OK;
    }

    if (s->len + len > sizeof(s->buf)) {
        sink_flush(s);
    }
    memcpy(s->buf + s->len, p, len);
    s->len += len;

    return CHC_OK;
}

static nativeSink*
sink_for(chdbChannel* helper) {
    nativeSink* s = palloc0(sizeof(*s));

    s->helper = helper;
    s->io     = (chc_io){ .ud = s, .write = sink_write };

    return s;
}

/*
 * Writes buffered rows to helper as one Native block. Sends data as encoder
 * produces it instead of copying entire block into another buffer first.
 */
static void
send_block(pgch_writer* w, nativeSink* sink) {
    chc_err err = {};

    if (chc_block_write(
            &sink->io, pgch_writer_build(w), &pgch_block_opts_local, &err
        ) != CHC_OK) {
        pgch_raise(&err, ERRCODE_EXTERNAL_ROUTINE_EXCEPTION, "block write: ", NULL);
    }
    pgch_writer_reset(w);
    sink_flush(sink);
}

/* table_beginscan gained a caller flags argument in PG 19. */
static inline TableScanDesc
begin_scan(Relation rel) {
#if PG_VERSION_NUM >= 190000
    return table_beginscan(rel, GetActiveSnapshot(), 0, NULL, 0);
#else
    return table_beginscan(rel, GetActiveSnapshot(), 0, NULL);
#endif
}

/* pgch_append_slot over `attnums` rather than every streamed attribute. */
static void
append_slot(pgch_writer* w, TupleTableSlot* slot, List* attnums) {
    TupleDesc desc = slot->tts_tupleDescriptor;
    size_t col     = 0;

    slot_getallattrs(slot);
    ListCell* lc;
    foreach (lc, attnums) {
        int i = lfirst_int(lc) - 1;

        pgch_append_datum(
            w,
            col++,
            slot->tts_values[i],
            TupleDescAttr(desc, i)->atttypid,
            slot->tts_isnull[i]
        );
    }
}

uint64_t
chdb_copy_send(
    Relation rel,
    const char* structure,
    List* attnums,
    chdbChannel* helper
) {
    pgch_writer* w   = writer_for(structure, list_length(attnums));
    nativeSink* sink = sink_for(helper);
    MemoryContext rowcxt =
        AllocSetContextCreate(CurrentMemoryContext, "chdb row", ALLOCSET_DEFAULT_SIZES);
    TableScanDesc scan   = begin_scan(rel);
    TupleTableSlot* slot = table_slot_create(rel, NULL);
    uint64_t rows        = 0;

    /*
     * A NULL array has no ClickHouse representation, and a nullable array
     * column is ordinary in Postgres, so store the empty array rather than
     * failing the load on one.
     */
    pgch_writer_set_null_array(w, PGCH_NULL_ARRAY_EMPTY);

    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        /* Appends copy into the writer's own context; detoasts land here. */
        MemoryContext oldcxt = MemoryContextSwitchTo(rowcxt);

        CHECK_FOR_INTERRUPTS();
        append_slot(w, slot, attnums);
        MemoryContextSwitchTo(oldcxt);
        MemoryContextReset(rowcxt);
        rows++;

        if (pgch_writer_bytes(w) >= CHDB_NATIVE_BLOCK_BYTES) {
            send_block(w, sink);
        }
    }

    /* Rows the scan left short of a cut, so one block, not one per row. */
    if (pgch_writer_rows(w)) {
        send_block(w, sink);
    }

    ExecDropSingleTupleTableSlot(slot);
    table_endscan(scan);
    pgch_writer_free(w);
    MemoryContextDelete(rowcxt);

    return rows;
}
