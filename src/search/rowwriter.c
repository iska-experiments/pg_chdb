/*
 * The Native row writer shared by every write path: packs (ctid, xmin, values)
 * into a pgch writer whose columns follow the index's ClickHouse table, and
 * converts between heap TIDs and the UInt64 ctid column.
 */

#include "postgres.h"

#include "access/relation.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "pg-clickhouse-encode.h"

#include "../native_writer.h"
#include "search.h"

/* Matches src/native.c: ClickHouse coalesces small blocks itself. */
#define BLOCK_BYTES (8 * 1024 * 1024)
/* ---- TID packing ---- */

uint64
chdb_search_tid_to_u64(ItemPointer tid) {
    return ((uint64)ItemPointerGetBlockNumber(tid) << 16) |
           ItemPointerGetOffsetNumber(tid);
}

void
chdb_search_u64_to_tid(uint64 v, ItemPointer tid) {
    ItemPointerSet(tid, (BlockNumber)(v >> 16), (OffsetNumber)(v & 0xffff));
}

/* ---- row writer ---- */

/*
 * Date32 and DateTime64 have no infinities: the encoder wraps an infinite
 * date into some finite one and lets a -infinity timestamp through, so a
 * row holding one would be found by the wrong comparisons for good.
 */
static void
check_finite(Datum value, Oid typid) {
    bool finite = true;

    if (typid == DATEOID) {
        finite = !DATE_NOT_FINITE(DatumGetDateADT(value));
    } else if (typid == TIMESTAMPOID || typid == TIMESTAMPTZOID) {
        finite = !TIMESTAMP_NOT_FINITE(DatumGetTimestamp(value));
    }
    if (!finite) {
        ereport(
            ERROR,
            errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
            errmsg(
                "an infinite %s cannot be stored in a chdb index", format_type_be(typid)
            )
        );
    }
}

struct ChdbRowWriter {
    pgch_writer* w;
    MemoryContext cxt;
    MemoryContext rowcxt;
    int natts;
    ChdbColumn* cols;
};

/* The table's structure names the columns, as COPY's does (src/native.c). */
ChdbRowWriter*
chdb_rowwriter_new(Relation index) {
    ChdbRowWriter* rw = palloc0(sizeof(*rw));

    rw->cxt = CurrentMemoryContext;
    rw->rowcxt =
        AllocSetContextCreate(rw->cxt, "chdb_search row", ALLOCSET_DEFAULT_SIZES);
    rw->natts = index->rd_att->natts;
    rw->cols  = chdb_search_columns(index);
    rw->w = chdb_writer_for(rw->cxt, chdb_search_structure(rw->cols, rw->natts), NULL);

    /* Nullable arrays are ordinary in Postgres, ClickHouse has no NULL array. */
    pgch_writer_set_null_array(rw->w, PGCH_NULL_ARRAY_EMPTY);
    return rw;
}

char*
chdb_rowwriter_column_list(ChdbRowWriter* rw) {
    return chdb_search_column_list(rw->cols, rw->natts);
}

void
chdb_rowwriter_append(
    ChdbRowWriter* rw,
    ItemPointer tid,
    TransactionId xmin,
    Datum* values,
    bool* isnull
) {
    MemoryContext old = MemoryContextSwitchTo(rw->rowcxt);

    pgch_append_datum(
        rw->w, 0, Int64GetDatum((int64)chdb_search_tid_to_u64(tid)), INT8OID, false
    );
    pgch_append_datum(rw->w, 1, Int32GetDatum((int32)xmin), INT4OID, false);
    for (int i = 0; i < rw->natts; i++) {
        if (!isnull[i]) {
            check_finite(values[i], rw->cols[i].typid);
        }
        pgch_append_datum(rw->w, i + 2, values[i], rw->cols[i].typid, isnull[i]);
    }
    MemoryContextSwitchTo(old);
    MemoryContextReset(rw->rowcxt);
}

void
chdb_rowwriter_checkpoint(ChdbRowWriter* rw, pgch_checkpoint* ckpt) {
    pgch_writer_checkpoint(rw->w, ckpt);
}

void
chdb_rowwriter_rollback(ChdbRowWriter* rw, const pgch_checkpoint* ckpt) {
    pgch_writer_rollback(rw->w, ckpt);
}

/*
 * A rollback bumps the writer's generation and later rollbacks refuse any
 * older checkpoint as stale. A checkpoint taken before the one rewound to
 * is still a prefix of the writer's columns, so it is stamped current. The
 * writer is opaque here, and a fresh checkpoint is how its generation reads.
 */
void
chdb_rowwriter_revalidate(ChdbRowWriter* rw, pgch_checkpoint* ckpt) {
    pgch_checkpoint now = {};

    pgch_writer_checkpoint(rw->w, &now);
    ckpt->generation = now.generation;
    pgch_checkpoint_free(&now);
}

size_t
chdb_rowwriter_bytes(ChdbRowWriter* rw) {
    return pgch_writer_bytes(rw->w);
}

size_t
chdb_rowwriter_rows(ChdbRowWriter* rw) {
    return pgch_writer_rows(rw->w);
}

void*
chdb_rowwriter_take(ChdbRowWriter* rw, size_t* len) {
    pgch_buf out = {};

    pgch_writer_flush(rw->w, &out, NULL);
    *len = out.len;
    return out.data;
}

void
chdb_rowwriter_free(ChdbRowWriter* rw) {
    pgch_writer_free(rw->w);
    MemoryContextDelete(rw->rowcxt);
    pfree(rw);
}
