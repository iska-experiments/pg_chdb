/*
 * VACUUM. The store holds every ctid ever inserted, so ambulkdelete asks the
 * callback about each one, and the rows of transactions that never
 * committed (xmin.c), whose TIDs the heap may have given out again, and
 * removes the dead ones with a DELETE run as a lightweight update: ClickHouse patches the parts holding the rows, found
 * by the block number and offset columns the table keeps (ddl.c), and never
 * rewrites a part with a mutation, which the Phase 1 plain_rewritable disk
 * rejects ("Mutations are not supported for immutable disk") and which the
 * default mode falls back to. amvacuumcleanup runs OPTIMIZE ... FINAL when
 * enough rows died to be worth rewriting parts: the patches only mask rows
 * until a merge. It also sweeps the tables of other generations, which a
 * rebuild leaves behind (see ddl.c), and the staging tables of transactions
 * that are over (see staging.c).
 */

#include "postgres.h"

#include <stdlib.h>

#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "commands/vacuum.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "query.h"
#include "search.h"
#include "stream.h"

/* ctids per DELETE, bounding statement size. */
#define DELETE_BATCH 10000

typedef struct VacuumStats {
    IndexBulkDeleteResult base;
    /*
     * Rows the last call found live. A VACUUM whose dead TIDs overflow
     * maintenance_work_mem calls ambulkdelete once per round, and each round
     * reads the whole store, so a count summed across them would be several
     * times the index's size; tuples_removed is cumulative, as Postgres has it.
     */
    double live;
} VacuumStats;

static void
delete_batch(
    Oid indexoid,
    uint64 generation,
    const char* table,
    uint64* dead,
    size_t n
) {
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfo(&buf, "DELETE FROM %s WHERE ctid IN (", table);
    for (size_t i = 0; i < n; i++) {
        appendStringInfo(&buf, "%s" UINT64_FORMAT, i ? "," : "", dead[i]);
    }
    appendStringInfoString(
        &buf, ") SETTINGS lightweight_delete_mode = 'lightweight_update_force'"
    );
    chdb_search_run(indexoid, generation, buf.data);
}

IndexBulkDeleteResult*
chdb_search_ambulkdelete(
    IndexVacuumInfo* info,
    IndexBulkDeleteResult* stats,
    IndexBulkDeleteCallback callback,
    void* callback_state
) {
    Relation index    = info->index;
    Oid oid           = RelationGetRelid(index);
    VacuumStats* vs   = (VacuumStats*)stats;
    MemoryContext cxt = AllocSetContextCreate(
        CurrentMemoryContext, "chdb_search vacuum", ALLOCSET_DEFAULT_SIZES
    );
    MemoryContext old;
    uint64* dead = NULL;
    size_t ndead = 0, cap = 0;
    ItemPointerData tid;
    TransactionId xmin;

    bool skip;

    /*
     * Deleting against a store this server cannot trust would corrupt it. In
     * skip mode the store is left alone, and so are the statistics: a result
     * for a store that was never read would put its row count at zero, where
     * none keeps the heap's estimate (amvacuumcleanup).
     */
    chdb_search_check_available(index, &skip);
    if (skip) {
        MemoryContextDelete(cxt);
        return stats;
    }
    /* The result outlives this call, the working memory does not. */
    if (!vs) {
        vs = palloc0(sizeof(*vs));
    }
    old = MemoryContextSwitchTo(cxt);

    uint64 generation = chdb_meta_generation(index);
    char* table       = chdb_search_table_of(oid, generation);

    /* Read everything before deleting: one connection cannot do both. */
    ChdbStream* s = chdb_search_stream_open(
        oid,
        generation,
        chdb_search_build_select(index, NULL, 0, NULL, 0, -1),
        0,
        0,
        cxt
    );

    vs->live = 0;
    while (chdb_search_stream_next(s, &tid, &xmin)) {
        if (!callback(&tid, callback_state) &&
            !chdb_search_xmin_aborted(info->heaprel, xmin)) {
            vs->live++;
        } else {
            if (ndead == cap) {
                cap  = cap ? cap * 2 : 1024;
                dead = dead ? repalloc_huge(dead, cap * sizeof(uint64))
                            : palloc_extended(cap * sizeof(uint64), MCXT_ALLOC_HUGE);
            }
            dead[ndead++] = chdb_search_tid_to_u64(&tid);
        }
        vacuum_delay_point(false);
    }
    chdb_search_stream_close(s);

    for (size_t i = 0; i < ndead; i += DELETE_BATCH) {
        delete_batch(oid, generation, table, dead + i, Min(DELETE_BATCH, ndead - i));
        vacuum_delay_point(false);
    }
    vs->base.tuples_removed += ndead;
    vs->base.num_pages = RelationGetNumberOfBlocks(index);

    MemoryContextSwitchTo(old);
    MemoryContextDelete(cxt);
    return &vs->base;
}

/*
 * Parses t_<generation> or t_<generation>_tx_<fxid>, the latter with a
 * non-zero fxid. Anything else in the database is not ours to drop.
 */
static bool
parse_table_name(const char* name, uint64* generation, uint64* fxid) {
    char* end;

    *fxid = 0;
    if (strncmp(name, "t_", 2) != 0) {
        return false;
    }
    *generation = strtoull(name + 2, &end, 10);
    if (end == name + 2) {
        return false;
    }
    if (*end == '\0') {
        return true;
    }
    if (strncmp(end, "_tx_", 4) != 0) {
        return false;
    }
    *fxid = strtoull(end + 4, &end, 10);
    return *end == '\0' && *fxid != 0;
}

/*
 * A staging table is stale once its transaction is over. The backend's own
 * transaction is not; one of an earlier epoch is, whatever its xid; a very
 * new xid reads as in progress, which errs on the side of keeping.
 */
static bool
staging_is_stale(uint64 fxid) {
    FullTransactionId v    = FullTransactionIdFromU64(fxid);
    FullTransactionId mine = GetTopFullTransactionIdIfAny();

    if (FullTransactionIdIsValid(mine) && FullTransactionIdEquals(mine, v)) {
        return false;
    }
    if (EpochFromFullTransactionId(v) !=
        EpochFromFullTransactionId(ReadNextFullTransactionId())) {
        return true;
    }
    return !TransactionIdIsInProgress(XidFromFullTransactionId(v));
}

/*
 * Drops the tables of every generation but the metapage's, which a rebuild
 * left behind: the old one after a committed rebuild, the new one after a
 * rolled-back rebuild whose abort callback never ran (a crashed backend).
 * Also drops the staging tables of transactions that are over, which the
 * same crash left. Names are read in full before any drop, as one
 * connection cannot stream and run statements at once, and a drop that
 * fails warns rather than failing the VACUUM.
 */
static void
sweep_tables(Relation index) {
    Oid oid           = RelationGetRelid(index);
    uint64 generation = chdb_meta_generation(index);
    MemoryContext cxt = AllocSetContextCreate(
        CurrentMemoryContext, "chdb_search sweep", ALLOCSET_SMALL_SIZES
    );
    MemoryContext old = MemoryContextSwitchTo(cxt);
    List* stale       = NIL;
    ListCell* lc;

    /* Cleanup names no generation: it must run with the current table gone too. */
    ChdbStream* s = chdb_search_stream_query(
        oid,
        0,
        psprintf(
            "SELECT name FROM system.tables WHERE database = 'idx_%u' "
            "AND name LIKE 't\\\\_%%'",
            oid
        ),
        (Oid[]){ TEXTOID },
        1,
        cxt
    );

    while (chdb_search_stream_next(s, NULL, NULL)) {
        char* name = TextDatumGetCString(s->vals[0]);
        uint64 found, fxid;

        if (parse_table_name(name, &found, &fxid) &&
            (fxid ? staging_is_stale(fxid) : found != generation)) {
            stale = lappend(stale, pstrdup(name));
        }
    }
    chdb_search_stream_close(s);

    foreach (lc, stale) {
        chdb_search_try_run(
            oid,
            0,
            psprintf("DROP TABLE IF EXISTS idx_%u.%s SYNC", oid, (char*)lfirst(lc))
        );
    }
    MemoryContextSwitchTo(old);
    MemoryContextDelete(cxt);
}

IndexBulkDeleteResult*
chdb_search_amvacuumcleanup(IndexVacuumInfo* info, IndexBulkDeleteResult* stats) {
    VacuumStats* vs = (VacuumStats*)stats;

    if (info->analyze_only) {
        return stats;
    }
    sweep_tables(info->index);
    if (!vs) {
        /*
         * No dead heap tuples, so ambulkdelete was skipped, or it left an
         * unavailable store alone: nothing to merge, and the heap's estimate
         * stands in for a count of the store's rows.
         */
        vs                        = palloc0(sizeof(*vs));
        vs->base.num_pages        = RelationGetNumberOfBlocks(info->index);
        vs->base.num_index_tuples = info->num_heap_tuples;
        vs->base.estimated_count  = true;
        return &vs->base;
    }

    /* What the store held before this VACUUM: the live rows plus the removed. */
    double held = vs->live + vs->base.tuples_removed;

    vs->base.num_index_tuples = vs->live;
    if (vs->base.tuples_removed > 0 && held > 0) {
        double ratio = chdb_search_index_optimize_ratio(info->index);

        if (vs->base.tuples_removed / held >= ratio) {
            uint64 generation = chdb_meta_generation(info->index);

            chdb_search_run(
                RelationGetRelid(info->index),
                generation,
                psprintf(
                    "OPTIMIZE TABLE %s FINAL",
                    chdb_search_table_of(RelationGetRelid(info->index), generation)
                )
            );
        }
    }
    return stats;
}
