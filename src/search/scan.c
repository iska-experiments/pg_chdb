/*
 * Index scans: one ClickHouse SELECT per scan, streamed back as Native blocks
 * and decoded row by row with the pgch reader.
 *
 * amgettuple fills xs_heaptid from the packed UInt64 ctid ((block << 16) |
 * offset) and the order-by values from the distance columns. xs_recheck is
 * false: ClickHouse has already applied the quals, and the heap fetch still
 * decides visibility, so rows of dead or rolled-back tuples that linger in the
 * store until VACUUM are harmless.
 */

#include "postgres.h"

#include "access/relscan.h"
#include "catalog/pg_type_d.h"
#include "nodes/tidbitmap.h"
#include "pgstat.h"
#include "utils/memutils.h"

#include "../native.h"
#include "query.h"
#include "search.h"

typedef struct ScanOpaque {
    MemoryContext cxt;
    ChdbStream* stream;
    bool started;
} ScanOpaque;

ChdbStream*
chdb_search_stream_open(Oid indexoid, const char* sql, int ndist, MemoryContext cxt) {
    MemoryContext old = MemoryContextSwitchTo(cxt);
    ChdbStream* s     = palloc0(sizeof(*s));

    s->cxt    = cxt;
    s->rowcxt = AllocSetContextCreate(cxt, "chdb_search row", ALLOCSET_SMALL_SIZES);
    s->conn   = chdb_search_connect();

    chdb_search_log_sql("select", sql);
    PG_TRY();
    {
        chdb_search_select(s->conn, indexoid, sql);

        pgch_block_source src = chdb_native_source(chdb_search_helper(s->conn));

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
        /* No block at all, not even an empty one: no rows. */
        s->done = true;
    } else {
        if (s->ncols != 1 + ndist) {
            ereport(
                ERROR,
                errcode(ERRCODE_DATA_EXCEPTION),
                errmsg(
                    "chdb_search: worker returned %d columns, expected %d",
                    s->ncols,
                    1 + ndist
                )
            );
        }
        s->states = palloc0(sizeof(void*) * s->ncols);
        s->vals   = palloc(sizeof(Datum) * s->ncols);
        s->nulls  = palloc(sizeof(bool) * s->ncols);
        for (int i = 0; i < s->ncols; i++) {
            s->states[i] = pgch_reader_convert_init(
                &s->reader, i, i == 0 ? INT8OID : FLOAT8OID, -1
            );
        }
    }
    MemoryContextSwitchTo(old);
    return s;
}

bool
chdb_search_stream_next(ChdbStream* s, ItemPointer tid) {
    if (!s->done) {
        MemoryContextReset(s->rowcxt);

        MemoryContext old = MemoryContextSwitchTo(s->rowcxt);
        bool more         = pgch_reader_next(&s->reader);

        if (more) {
            pgch_reader_fill(&s->reader, s->states, s->vals, s->nulls);
        }
        MemoryContextSwitchTo(old);

        if (more) {
            chdb_search_u64_to_tid((uint64)DatumGetInt64(s->vals[0]), tid);
            return true;
        }
        if (s->reader.error) {
            ereport(
                ERROR,
                errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                errmsg("chdb_search: %s", s->reader.error)
            );
        }
        s->done = true;
        chdb_search_finish(s->conn);
        chdb_search_close(s->conn);
        s->conn = NULL;
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

IndexScanDesc
chdb_search_ambeginscan(Relation index, int nkeys, int norderbys) {
    IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
    ScanOpaque* so     = palloc0(sizeof(*so));

    so->cxt = AllocSetContextCreate(
        CurrentMemoryContext, "chdb_search scan", ALLOCSET_DEFAULT_SIZES
    );
    scan->opaque = so;
    return scan;
}

static void
reset_stream(ScanOpaque* so) {
    if (so->stream) {
        chdb_search_stream_close(so->stream);
        so->stream = NULL;
    }
    MemoryContextReset(so->cxt);
    so->started = false;
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
    char* sql      = chdb_search_build_select(
        scan->indexRelation,
        scan->keyData,
        scan->numberOfKeys,
        scan->orderByData,
        scan->numberOfOrderBys,
        -1
    );

    so->started = true;
    pgstat_count_index_scan(scan->indexRelation);
    if (scan->instrument) {
        scan->instrument->nsearches++;
    }
    if (sql) {
        so->stream = chdb_search_stream_open(
            RelationGetRelid(scan->indexRelation), sql, scan->numberOfOrderBys, so->cxt
        );
    }
}

bool
chdb_search_amgettuple(IndexScanDesc scan, ScanDirection dir) {
    ScanOpaque* so = scan->opaque;

    if (!so->started) {
        start(scan);
    }
    if (!so->stream || !chdb_search_stream_next(so->stream, &scan->xs_heaptid)) {
        return false;
    }

    scan->xs_recheck = false;
    for (int i = 0; i < scan->numberOfOrderBys; i++) {
        scan->xs_orderbyvals[i]  = so->stream->vals[1 + i];
        scan->xs_orderbynulls[i] = so->stream->nulls[1 + i];
    }
    scan->xs_recheckorderby = false;
    return true;
}

int64
chdb_search_amgetbitmap(IndexScanDesc scan, TIDBitmap* tbm) {
    ScanOpaque* so = scan->opaque;
    ItemPointerData tid;
    int64 n = 0;

    if (!so->started) {
        start(scan);
    }
    while (so->stream && chdb_search_stream_next(so->stream, &tid)) {
        tbm_add_tuples(tbm, &tid, 1, false);
        n++;
    }
    return n;
}

void
chdb_search_amendscan(IndexScanDesc scan) {
    ScanOpaque* so = scan->opaque;

    reset_stream(so);
    MemoryContextDelete(so->cxt);
}
