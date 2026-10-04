/*
 * A SELECT run against the worker and read row by row: the Native blocks
 * it streams back are decoded by the pgch reader into Datums of the types
 * the caller names. A scan reads the packed ctid, the transaction id, the
 * distances and the scores (scan.c, planner/exec.c), and asks here whether
 * the heap may be asked for a row's tuple; VACUUM reads every ctid, and its
 * sweep the names of the store's tables (vacuum.c); the score's counts and
 * the aggregate scan read what they select. See stream.h.
 */

#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"

#include "../native.h"
#include "search.h"
#include "stream.h"

/* Reads the request's status, which raises the worker's error, and closes. */
static void
end_stream(ChdbStream* s) {
    s->done = true;
    chdb_search_finish(s->conn);
    chdb_search_close(s->conn);
    s->conn = NULL;
}

ChdbStream*
chdb_search_stream_query(
    Oid indexoid,
    uint64 generation,
    const char* sql,
    const Oid* types,
    int ncols,
    MemoryContext cxt
) {
    MemoryContext old = MemoryContextSwitchTo(cxt);
    ChdbStream* s     = palloc0(sizeof(*s));

    s->cxt    = cxt;
    s->rowcxt = AllocSetContextCreate(cxt, "chdb_search row", ALLOCSET_SMALL_SIZES);
    s->conn   = chdb_search_connect();

    chdb_search_log_sql("select", sql);
    PG_TRY();
    {
        chdb_search_select(s->conn, indexoid, generation, sql);

        pgch_block_source src = chdb_native_source(chdb_search_channel(s->conn));

        pgch_reader_init(&s->reader, &src);
        if (s->reader.error) {
            ereport(
                ERROR,
                errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                errmsg("chdb_search: %s", s->reader.error)
            );
        }
    }
    PG_CATCH();
    {
        chdb_search_close(s->conn);
        PG_RE_THROW();
    }
    PG_END_TRY();

    s->ncols = (int)pgch_reader_columns(&s->reader);
    if (s->ncols == 0) {
        /* No block at all, not even an empty one: no rows, or a failed query. */
        end_stream(s);
    } else {
        if (s->ncols != ncols) {
            ereport(
                ERROR,
                errcode(ERRCODE_DATA_EXCEPTION),
                errmsg(
                    "chdb_search: worker returned %d columns, expected %d",
                    s->ncols,
                    ncols
                )
            );
        }
        s->states = palloc0(sizeof(void*) * s->ncols);
        s->vals   = palloc(sizeof(Datum) * s->ncols);
        s->nulls  = palloc(sizeof(bool) * s->ncols);
        for (int i = 0; i < s->ncols; i++) {
            s->states[i] = pgch_reader_convert_init(&s->reader, i, types[i], -1);
        }
    }
    MemoryContextSwitchTo(old);
    return s;
}

/*
 * A scan's stream: the packed ctid and the transaction id, then `ndist`
 * distances and `nscores` scores.
 */
ChdbStream*
chdb_search_stream_open(
    Oid indexoid,
    uint64 generation,
    const char* sql,
    int ndist,
    int nscores,
    MemoryContext cxt
) {
    int ncols  = 2 + ndist + nscores;
    Oid* types = palloc(sizeof(Oid) * ncols);

    types[0] = INT8OID;
    types[1] = INT8OID;
    for (int i = 2; i < ncols; i++) {
        types[i] = i < 2 + ndist ? FLOAT8OID : FLOAT4OID;
    }
    return chdb_search_stream_query(indexoid, generation, sql, types, ncols, cxt);
}

bool
chdb_search_stream_next(ChdbStream* s, ItemPointer tid, TransactionId* xmin) {
    if (!s->done) {
        MemoryContextReset(s->rowcxt);

        MemoryContext old = MemoryContextSwitchTo(s->rowcxt);
        bool more         = pgch_reader_next(&s->reader);

        if (more) {
            pgch_reader_fill(&s->reader, s->states, s->vals, s->nulls);
        }
        MemoryContextSwitchTo(old);

        if (more) {
            if (tid) {
                chdb_search_u64_to_tid((uint64)DatumGetInt64(s->vals[0]), tid);
            }
            if (xmin) {
                *xmin = (TransactionId)DatumGetInt64(s->vals[1]);
            }
            return true;
        }
        if (s->reader.error) {
            ereport(
                ERROR,
                errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                errmsg("chdb_search: %s", s->reader.error)
            );
        }
        end_stream(s);
    }
    return false;
}

void
chdb_search_stream_close(ChdbStream* s) {
    if (s->conn) {
        chdb_search_close(s->conn);
        s->conn = NULL;
    }
    s->done = true;
    MemoryContextDelete(s->rowcxt);
}

/*
 * Whether `tid` points into the heap. A store from before the heap lost its
 * last pages, or copied from after they were allocated, names blocks past
 * the heap's end, and heapam reads such a block unconditionally and raises
 * "could not read blocks". The count, kept in the stream, is measured again
 * when a block is at or past it: any row the store can return had its block
 * allocated before the flush that sent it, so a fresh count only ever
 * filters phantoms.
 */
static bool
tid_in_heap(ChdbStream* s, Relation heap, ItemPointer tid) {
    BlockNumber blk = ItemPointerGetBlockNumber(tid);

    if (blk >= s->heap_nblocks) {
        s->heap_nblocks = RelationGetNumberOfBlocks(heap);
    }
    return blk < s->heap_nblocks;
}

/*
 * Whether the row's transaction ended without committing (xmin.c). A
 * transaction's rows come together, in ctid order, so the last verdict
 * answers for most rows.
 */
static bool
row_aborted(ChdbStream* s, Relation heap, TransactionId xmin) {
    if (xmin != s->last_xmin || !TransactionIdIsValid(xmin)) {
        s->last_xmin    = xmin;
        s->last_aborted = chdb_search_xmin_aborted(heap, xmin);
    }
    return s->last_aborted;
}

bool
chdb_search_stream_fetchable(ChdbStream* s, Relation heap, ItemPointer tid) {
    return tid_in_heap(s, heap, tid) &&
           !row_aborted(s, heap, (TransactionId)DatumGetInt64(s->vals[1]));
}
