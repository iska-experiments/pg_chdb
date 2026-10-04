/*
 * VACUUM. The store holds every ctid ever inserted, so ambulkdelete asks the
 * callback about each one and removes the dead ones with a lightweight
 * DELETE. amvacuumcleanup runs OPTIMIZE ... FINAL when enough rows died to be
 * worth rewriting parts: lightweight deletes only mask rows until a merge.
 */

#include "postgres.h"

#include "commands/vacuum.h"
#include "utils/memutils.h"

#include "query.h"
#include "search.h"

/* ctids per DELETE, bounding statement size. */
#define DELETE_BATCH 10000

typedef struct VacuumStats {
    IndexBulkDeleteResult base;
    double scanned;
} VacuumStats;

static void
delete_batch(Oid indexoid, uint64* dead, size_t n) {
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfo(
        &buf, "DELETE FROM %s WHERE ctid IN (", chdb_search_table_name(indexoid)
    );
    for (size_t i = 0; i < n; i++) {
        appendStringInfo(&buf, "%s" UINT64_FORMAT, i ? "," : "", dead[i]);
    }
    appendStringInfoChar(&buf, ')');
    chdb_search_run(indexoid, buf.data);
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

    /* The result outlives this call, the working memory does not. */
    if (!vs) {
        vs = palloc0(sizeof(*vs));
    }
    old = MemoryContextSwitchTo(cxt);

    /* Read everything before deleting: one connection cannot do both. */
    ChdbStream* s = chdb_search_stream_open(
        oid, chdb_search_build_select(index, NULL, 0, NULL, 0, -1), 0, cxt
    );

    while (chdb_search_stream_next(s, &tid)) {
        vs->scanned++;
        if (callback(&tid, callback_state)) {
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
        delete_batch(oid, dead + i, Min(DELETE_BATCH, ndead - i));
        vacuum_delay_point(false);
    }
    vs->base.tuples_removed += ndead;

    MemoryContextSwitchTo(old);
    MemoryContextDelete(cxt);
    return &vs->base;
}

IndexBulkDeleteResult*
chdb_search_amvacuumcleanup(IndexVacuumInfo* info, IndexBulkDeleteResult* stats) {
    VacuumStats* vs = (VacuumStats*)stats;

    if (info->analyze_only) {
        return stats;
    }
    if (!vs) {
        /* No dead heap tuples, so ambulkdelete was skipped and nothing to merge. */
        vs                        = palloc0(sizeof(*vs));
        vs->base.num_index_tuples = info->num_heap_tuples;
        vs->base.estimated_count  = true;
        return &vs->base;
    }

    vs->base.num_index_tuples = vs->scanned - vs->base.tuples_removed;
    if (vs->base.tuples_removed > 0 && vs->scanned > 0) {
        double ratio = chdb_search_index_optimize_ratio(info->index);

        if (vs->base.tuples_removed / vs->scanned >= ratio) {
            chdb_search_run(
                RelationGetRelid(info->index),
                psprintf(
                    "OPTIMIZE TABLE %s FINAL",
                    chdb_search_table_name(RelationGetRelid(info->index))
                )
            );
        }
    }
    return stats;
}
