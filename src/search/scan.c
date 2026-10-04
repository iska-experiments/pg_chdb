/*
 * Index scans: one ClickHouse SELECT per scan, streamed back as Native blocks
 * and decoded row by row with the pgch reader.
 *
 * amgettuple fills xs_heaptid from the packed UInt64 ctid ((block << 16) |
 * offset) and the order-by values from the distance columns. xs_recheck is
 * false: ClickHouse has already applied the quals, and the heap fetch still
 * decides visibility, so rows of dead tuples that linger in the store until
 * VACUUM are harmless, but for the rows of a transaction that never
 * committed, whose TIDs the heap may have given to other rows since: those
 * are skipped by the transaction id each row carries (xmin.c). A ctid past
 * the heap's end is skipped rather than fetched, as heapam would raise on
 * it (chdb_search_stream_fetchable, which the custom scan shares).
 *
 * The index returns no column (amcanreturn is unset), yet the planner may
 * still choose an index-only scan of it when a query needs none, as count(*)
 * does, so a scan wanting an index tuple gets one with every column null,
 * which the executor stores and never reads: on an all-visible page each
 * store row is one live tuple, so the count is right.
 *
 * There is no amgetbitmap. A TIDBitmap past work_mem, or ANDed with a lossy
 * sibling, makes the bitmap heap scan recheck the quals with the Postgres
 * fallbacks of ops.c, which know the default tokenizer only, so every match
 * of an ngrams, splitByString, icu or extractTextFromHTML column on a lossy
 * page would be dropped. A bitmap scan would buy nothing anyway: the scan
 * streams the whole TID set either way. (pgvector ships none either.)
 */

#include "postgres.h"

#include "access/relscan.h"
#include "pgstat.h"
#include "utils/memutils.h"

#include "query.h"
#include "search.h"
#include "stream.h"

typedef struct ScanOpaque {
    MemoryContext cxt;
    ChdbStream* stream;
    bool started;
    IndexTuple null_itup; /* for an index-only scan, built on first use */
} ScanOpaque;

IndexScanDesc
chdb_search_ambeginscan(Relation index, int nkeys, int norderbys) {
    IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
    ScanOpaque* so     = palloc0(sizeof(*so));

    so->cxt = AllocSetContextCreate(
        CurrentMemoryContext, "chdb_search scan", ALLOCSET_DEFAULT_SIZES
    );
    scan->opaque = so;
    /* For an index-only scan's tuple; RelationGetIndexScan leaves it NULL. */
    scan->xs_itupdesc = RelationGetDescr(index);
    /* RelationGetIndexScan leaves these to the access method, as gist does. */
    if (norderbys > 0) {
        scan->xs_orderbyvals  = palloc0(sizeof(Datum) * norderbys);
        scan->xs_orderbynulls = palloc(sizeof(bool) * norderbys);
        memset(scan->xs_orderbynulls, true, sizeof(bool) * norderbys);
    }
    return scan;
}

static void
reset_stream(ScanOpaque* so) {
    if (so->stream) {
        chdb_search_stream_close(so->stream);
        so->stream = NULL;
    }
    MemoryContextReset(so->cxt);
    so->started   = false;
    so->null_itup = NULL;
}

void
chdb_search_amrescan(
    IndexScanDesc scan,
    ScanKey keys,
    int nkeys,
    ScanKey orderbys,
    int norderbys
) {
    ScanOpaque* so = scan->opaque;

    reset_stream(so);
    if (keys && scan->numberOfKeys > 0) {
        memcpy(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
    }
    if (orderbys && scan->numberOfOrderBys > 0) {
        memcpy(
            scan->orderByData, orderbys, scan->numberOfOrderBys * sizeof(ScanKeyData)
        );
    }
}

/* Sends the query on first use. A NULL key leaves no stream: no rows. */
static void
start(IndexScanDesc scan) {
    ScanOpaque* so = scan->opaque;
    bool skip;

    /* Never answer from a store this server cannot prove current. */
    chdb_search_check_available(scan->indexRelation, &skip);
    if (skip) {
        so->started = true;
        return;
    }

    /* In the scan's context, which a rescan resets, not the executor's. */
    MemoryContext old = MemoryContextSwitchTo(so->cxt);
    char* sql         = chdb_search_build_select(
        scan->indexRelation,
        scan->keyData,
        scan->numberOfKeys,
        scan->orderByData,
        scan->numberOfOrderBys,
        -1
    );

    MemoryContextSwitchTo(old);
    so->started = true;
    pgstat_count_index_scan(scan->indexRelation);
#if PG_VERSION_NUM >= 180000
    if (scan->instrument) {
        scan->instrument->nsearches++;
    }
#endif
    if (sql) {
        so->stream = chdb_search_stream_open(
            RelationGetRelid(scan->indexRelation),
            chdb_meta_generation(scan->indexRelation),
            sql,
            scan->numberOfOrderBys,
            0,
            so->cxt
        );
    }
}

bool
chdb_search_amgettuple(IndexScanDesc scan, ScanDirection dir) {
    ScanOpaque* so = scan->opaque;

    if (!so->started) {
        start(scan);
    }
    if (!so->stream) {
        return false;
    }
    do {
        if (!chdb_search_stream_next(so->stream, &scan->xs_heaptid, NULL)) {
            return false;
        }
    } while (!chdb_search_stream_fetchable(
        so->stream, scan->heapRelation, &scan->xs_heaptid
    ));

    scan->xs_recheck = false;
    if (scan->xs_want_itup) {
        if (!so->null_itup) {
            TupleDesc desc = RelationGetDescr(scan->indexRelation);
            Datum* values  = palloc0(sizeof(Datum) * desc->natts);
            bool* isnull   = palloc(sizeof(bool) * desc->natts);
            MemoryContext old;

            memset(isnull, true, sizeof(bool) * desc->natts);
            old           = MemoryContextSwitchTo(so->cxt);
            so->null_itup = index_form_tuple(desc, values, isnull);
            MemoryContextSwitchTo(old);
        }
        scan->xs_itup = so->null_itup;
    }
    for (int i = 0; i < scan->numberOfOrderBys; i++) {
        scan->xs_orderbyvals[i]  = so->stream->vals[2 + i];
        scan->xs_orderbynulls[i] = so->stream->nulls[2 + i];
    }
    scan->xs_recheckorderby = false;
    return true;
}

void
chdb_search_amendscan(IndexScanDesc scan) {
    ScanOpaque* so = scan->opaque;

    reset_stream(so);
    MemoryContextDelete(so->cxt);
}
