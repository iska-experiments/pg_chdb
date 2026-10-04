/*
 * Running the aggregate scan. The store answers with the groups, which the
 * node holds in a tuplestore until the heap has vouched for them, then
 * returns one by one as the scan tuple; the target list and the HAVING
 * clauses evaluate on it through the executor's own projection and qual.
 *
 * The heap vouches when its visibility map says every page is all-visible
 * both before the statement is sent and after its answer is in. Any insert,
 * update or delete of a heap tuple clears the bit of its page before the
 * row can reach the store, and VACUUM sets a bit again only for tuples
 * every open snapshot sees, so two clean checks around the statement mean
 * the store's rows are the snapshot's rows. When either check fails, or the
 * store is unavailable in skip mode, the node runs the Agg plan it carries
 * as its child instead, which scans the heap under the snapshot, and the
 * answer is exact either way. EXPLAIN ANALYZE says which answered.
 */

#include "postgres.h"

#include <string.h>

#include "access/genam.h"
#include "access/table.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

#include "../search.h"
#include "../stream.h"
#include "planner.h"

static Node*
create_scan_state(CustomScan* cscan) {
    ChdbAggState* a = (ChdbAggState*)newNode(sizeof(ChdbAggState), T_CustomScanState);

    a->scan.css.methods = &chdb_planner_agg_exec_methods;
    /* The tuplestore's kind of tuple; the fills store virtual values in it. */
    a->scan.css.slotOps = &TTSOpsMinimalTuple;
    a->scan.spec        = chdb_planner_agg_unpack(cscan);
    return (Node*)a;
}

static void
begin_scan(CustomScanState* css, EState* estate, int eflags) {
    ChdbAggState* a   = (ChdbAggState*)css;
    ChdbScanState* st = &a->scan;
    CustomScan* cscan = (CustomScan*)css->ss.ps.plan;

    st->index = index_open(st->spec->indexoid, AccessShareLock);
    a->heap   = table_open(st->index->rd_index->indrelid, AccessShareLock);
    st->cxt   = AllocSetContextCreate(
        estate->es_query_cxt, "chdb_search aggregate scan", ALLOCSET_DEFAULT_SIZES
    );
    st->asked = -1;
    chdb_planner_init_args(st);
    a->exact       = ExecInitNode(linitial(cscan->custom_plans), estate, eflags);
    css->custom_ps = list_make1(a->exact);
}

/* Reads the store's answer into the tuplestore. */
static void
read_store(ChdbAggState* a, TupleTableSlot* slot) {
    ChdbScanState* st = &a->scan;
    int ncols;
    Oid* types        = chdb_planner_agg_types(st->spec, &ncols);
    MemoryContext old = MemoryContextSwitchTo(st->cxt);

    a->rows = tuplestore_begin_heap(false, false, work_mem);
    MemoryContextSwitchTo(old);
    st->stream = chdb_search_stream_query(
        RelationGetRelid(st->index),
        chdb_meta_generation(st->index),
        st->sql,
        types,
        ncols,
        st->cxt
    );
    st->store_queries++;
    while (chdb_search_stream_next(st->stream, NULL)) {
        chdb_planner_agg_fill(st->spec, st->stream, slot);
        tuplestore_puttupleslot(a->rows, slot);
        st->store_rows++;
    }
    chdb_search_stream_close(st->stream);
    st->stream = NULL;
}

/* Decides who answers: the store, checked around its statement, or the exact plan. */
static void
start(ChdbAggState* a, TupleTableSlot* slot) {
    ChdbScanState* st = &a->scan;
    bool skip;

    st->started = true;
    chdb_search_check_available(st->index, &skip);
    if (skip) {
        a->fallback = "the store is not available";
        return;
    }
    if (!chdb_planner_heap_all_visible(a->heap)) {
        a->fallback = "the heap is not all-visible";
        return;
    }
    if (!st->built) {
        chdb_planner_build_agg_sql(st);
    }
    pgstat_count_index_scan(st->index);
    read_store(a, slot);
    if (!chdb_planner_heap_all_visible(a->heap)) {
        tuplestore_end(a->rows);
        a->rows     = NULL;
        a->fallback = "the heap changed under the statement";
    }
}

/*
 * The exact plan's next group as the scan tuple: the outputs are its
 * columns, and the columns the pushed clauses name follow them.
 */
static TupleTableSlot*
exact_tuple(ChdbAggState* a, TupleTableSlot* slot) {
    TupleTableSlot* got = ExecProcNode(a->exact);
    int natts;

    if (TupIsNull(got)) {
        return ExecClearTuple(slot);
    }
    natts = got->tts_tupleDescriptor->natts;
    slot_getallattrs(got);
    ExecClearTuple(slot);
    memcpy(slot->tts_values, got->tts_values, sizeof(Datum) * natts);
    memcpy(slot->tts_isnull, got->tts_isnull, sizeof(bool) * natts);
    chdb_planner_agg_pad(slot, natts);
    return ExecStoreVirtualTuple(slot);
}

static TupleTableSlot*
next_tuple(ScanState* ss) {
    ChdbAggState* a      = (ChdbAggState*)ss;
    TupleTableSlot* slot = ss->ss_ScanTupleSlot;

    if (!a->scan.started) {
        start(a, slot);
    }
    if (a->fallback) {
        return exact_tuple(a, slot);
    }
    if (tuplestore_gettupleslot(a->rows, true, false, slot)) {
        return slot;
    }
    return ExecClearTuple(slot);
}

/*
 * EvalPlanQual: a scan of no relation gets no test tuple, and the executor
 * asks the recheck for the tuple instead, so this is the fetch.
 */
static bool
recheck(ScanState* ss, TupleTableSlot* slot) {
    return !TupIsNull(next_tuple(ss));
}

static TupleTableSlot*
exec_scan(CustomScanState* css) {
    return ExecScan(&css->ss, next_tuple, recheck);
}

static void
reset(ChdbAggState* a) {
    if (a->rows) {
        tuplestore_end(a->rows);
        a->rows = NULL;
    }
    a->fallback = NULL;
    chdb_planner_reset(&a->scan);
}

static void
rescan(CustomScanState* css) {
    ChdbAggState* a = (ChdbAggState*)css;

    reset(a);
    /* The child is this node's to tell about changed parameters, as a
     * SubqueryScan tells its subplan. */
    if (css->ss.ps.chgParam != NULL) {
        UpdateChangedParamSet(a->exact, css->ss.ps.chgParam);
    }
    if (a->exact->chgParam == NULL) {
        ExecReScan(a->exact);
    }
    ExecScanReScan(&css->ss);
}

static void
end_scan(CustomScanState* css) {
    ChdbAggState* a = (ChdbAggState*)css;

    reset(a);
    ExecEndNode(a->exact);
    table_close(a->heap, NoLock);
    index_close(a->scan.index, NoLock);
    MemoryContextDelete(a->scan.cxt);
}

const CustomScanMethods chdb_planner_agg_scan_methods = {
    .CustomName            = "chdb_search aggregate",
    .CreateCustomScanState = create_scan_state,
};

const CustomExecMethods chdb_planner_agg_exec_methods = {
    .CustomName        = "chdb_search aggregate",
    .BeginCustomScan   = begin_scan,
    .ExecCustomScan    = exec_scan,
    .EndCustomScan     = end_scan,
    .ReScanCustomScan  = rescan,
    .ExplainCustomScan = chdb_planner_agg_explain,
};
