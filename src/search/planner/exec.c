/*
 * Running the custom scan. The first call evaluates the pushed arguments
 * into scan keys and renders them through select.c, the builder the index
 * scan uses, so the two send the same statement; the store streams back
 * ctids, which the scan fetches from the heap through the table access
 * method under the executor's snapshot, as an index scan does: a row the
 * store still holds for a deleted or updated tuple is not returned, and
 * one past the heap's end is not read. The scan tuple is the heap tuple,
 * so the quals kept here and the target list evaluate on it as on a
 * sequential scan's, and EvalPlanQual rechecks the pushed clauses with
 * their Postgres implementations.
 *
 * A pushed LIMIT counts store rows, not visible ones: when the store
 * returned as many as asked and the heap hid some, the scan asks again
 * for twice as many, skipping the ctids it has seen, until the LIMIT is
 * met or the store runs out.
 *
 * With outputs, the columns the store computes (chdb.score), the scan tuple
 * is a virtual one shaped by the plan's custom_scan_tlist: the heap row is
 * fetched into a slot of its own and the columns the query needs are copied
 * from it, then the stream's values follow. The quals and the target list
 * were rewritten to that shape by setrefs.c.
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/tableam.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "pgstat.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "../query.h"
#include "../search.h"
#include "../stream.h"
#include "planner.h"

static Node*
create_scan_state(CustomScan* cscan) {
    ChdbScanState* st =
        (ChdbScanState*)newNode(sizeof(ChdbScanState), T_CustomScanState);

    st->css.methods = &chdb_planner_exec_methods;
    st->spec        = chdb_planner_unpack(cscan);
    /*
     * The heap tuple itself, no copy, and the quals deform it as they need;
     * or, with outputs, a virtual tuple of the custom_scan_tlist's shape.
     */
    st->css.slotOps = st->spec->outputs ? &TTSOpsVirtual : &TTSOpsBufferHeapTuple;
    return (Node*)st;
}

/* Whether an argument waits on a subplan, which plain EXPLAIN never runs. */
static bool
exec_param_walker(Node* node, void* context) {
    if (!node) {
        return false;
    }
    if (IsA(node, Param) && ((Param*)node)->paramkind == PARAM_EXEC) {
        return true;
    }
    return expression_tree_walker(node, exec_param_walker, context);
}

void
chdb_planner_init_args(ChdbScanState* st) {
    PlanState* ps = &st->css.ss.ps;
    List* clauses = NIL;
    ListCell* lc;

    foreach (lc, list_concat_copy(st->spec->quals, st->spec->orderbys)) {
        ChdbPushed* p = lfirst(lc);
        Expr* arg     = chdb_pushed_arg(p);

        st->args = lappend(st->args, ExecInitExpr(arg, ps));
        st->exec_params |= exec_param_walker((Node*)arg, NULL);
    }
    /* The predicates only: an order-by's distance is no boolean. */
    foreach (lc, st->spec->quals) {
        clauses = lappend(clauses, ((ChdbPushed*)lfirst(lc))->clause);
    }
    st->recheck = ExecInitQual(clauses, ps);
}

static void
begin_scan(CustomScanState* css, EState* estate, int eflags) {
    ChdbScanState* st = (ChdbScanState*)css;
    Relation heap     = css->ss.ss_currentRelation;

    if (table_slot_callbacks(heap) != &TTSOpsBufferHeapTuple) {
        elog(ERROR, "chdb custom scan on a relation that is not a heap table");
    }
    st->index = index_open(st->spec->indexoid, AccessShareLock);
    st->cxt   = AllocSetContextCreate(
        estate->es_query_cxt, "chdb_search custom scan", ALLOCSET_DEFAULT_SIZES
    );
    st->asked = -1;
    chdb_planner_init_args(st);
    if (st->spec->outputs) {
        List* tlist = ((CustomScan*)css->ss.ps.plan)->custom_scan_tlist;

        st->heap_slot = table_slot_create(heap, &estate->es_tupleTable);
        st->nvars     = list_length(tlist) - list_length(st->spec->outputs);
        st->attnos    = palloc(sizeof(AttrNumber) * st->nvars);
        for (int i = 0; i < st->nvars; i++) {
            st->attnos[i] =
                castNode(Var, list_nth_node(TargetEntry, tlist, i)->expr)->varattno;
        }
        /* For the whole statement: a rescan with other needles asks less. */
        st->scores = chdb_search_score_cache(estate->es_query_cxt);
    }
    if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY)) {
#if PG_VERSION_NUM >= 190000
        st->fetch = table_index_fetch_begin(heap, 0);
#else
        st->fetch = table_index_fetch_begin(heap);
#endif
    }
}

/*
 * The scan tuple from the fetched heap row and the stream's row: the heap
 * columns the plan refers to, system columns and the whole row among them,
 * then the outputs behind the ctid and the distances.
 */
static void
fill_scan_slot(ChdbScanState* st, TupleTableSlot* slot) {
    int ndist = list_length(st->spec->orderbys);

    ExecClearTuple(slot);
    for (int i = 0; i < st->nvars; i++) {
        AttrNumber attno = st->attnos[i];

        if (attno > 0) {
            slot->tts_values[i] =
                slot_getattr(st->heap_slot, attno, &slot->tts_isnull[i]);
        } else if (attno < 0) {
            slot->tts_values[i] =
                slot_getsysattr(st->heap_slot, attno, &slot->tts_isnull[i]);
        } else {
            ExprContext* econtext = st->css.ss.ps.ps_ExprContext;
            MemoryContext old = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

            slot->tts_values[i] = ExecFetchSlotHeapTupleDatum(st->heap_slot);
            slot->tts_isnull[i] = false;
            MemoryContextSwitchTo(old);
        }
    }
    for (int i = 0; i < list_length(st->spec->outputs); i++) {
        slot->tts_values[st->nvars + i] = st->stream->vals[1 + ndist + i];
        slot->tts_isnull[st->nvars + i] = st->stream->nulls[1 + ndist + i];
    }
    ExecStoreVirtualTuple(slot);
}

static void
open_stream(ChdbScanState* st) {
    st->stream = chdb_search_stream_open(
        RelationGetRelid(st->index),
        chdb_meta_generation(st->index),
        st->sql,
        list_length(st->spec->orderbys),
        list_length(st->spec->outputs),
        st->cxt
    );
    st->store_queries++;
    st->got = 0;
}

/* Sends the statement. Never from a store this server cannot prove current. */
static void
start(ChdbScanState* st) {
    st->started = true;
    chdb_search_check_available(st->index, &st->skip);
    if (st->skip) {
        return;
    }
    if (!st->built) {
        chdb_planner_build_sql(st, st->spec->limit);
    }
    pgstat_count_index_scan(st->index);
    if (st->sql) {
        open_stream(st);
    }
    if (st->spec->limit >= 0) {
        HASHCTL ctl = { .keysize   = sizeof(uint64),
                        .entrysize = sizeof(uint64),
                        .hcxt      = st->cxt };

        st->seen = hash_create(
            "chdb_search scan ctids", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT
        );
    }
}

/* Records a ctid the store returned; false when it had returned it before. */
static bool
first_time(ChdbScanState* st, ItemPointer tid) {
    uint64 key = chdb_search_tid_to_u64(tid);
    bool found;

    hash_search(st->seen, &key, HASH_ENTER, &found);
    return !found;
}

/*
 * At the end of the store's rows: whether to ask for more. A LIMIT the
 * store filled whose rows the heap did not all show has more behind it.
 */
static bool
ask_again(ChdbScanState* st) {
    bool more =
        st->asked >= 0 && st->got == st->asked && st->returned < st->spec->limit;

    chdb_search_stream_close(st->stream);
    st->stream = NULL;
    if (!more) {
        return false;
    }
    chdb_planner_build_sql(st, st->asked * 2);
    open_stream(st);
    return true;
}

static TupleTableSlot*
next_tuple(ScanState* ss) {
    ChdbScanState* st    = (ChdbScanState*)ss;
    TupleTableSlot* slot = ss->ss_ScanTupleSlot;
    TupleTableSlot* heap = st->heap_slot ? st->heap_slot : slot;
    Snapshot snapshot    = ss->ps.state->es_snapshot;
    ItemPointerData tid;

    if (!st->started) {
        start(st);
    }
    while (st->stream) {
        bool call_again = false, all_dead = false;

        if (!chdb_search_stream_next(st->stream, &tid)) {
            if (!ask_again(st)) {
                break;
            }
            continue;
        }
        st->got++;
        st->store_rows++;
        if (!chdb_search_tid_in_heap(ss->ss_currentRelation, &st->heap_nblocks, &tid) ||
            (st->seen && !first_time(st, &tid))) {
            continue;
        }
        /* Under an MVCC snapshot one version at most is visible: no call_again. */
        if (table_index_fetch_tuple(
                st->fetch, &tid, snapshot, heap, &call_again, &all_dead
            )) {
            if (heap != slot) {
                fill_scan_slot(st, slot);
            }
            st->returned++;
            return slot;
        }
    }
    return ExecClearTuple(slot);
}

/* EvalPlanQual: the pushed clauses, as Postgres evaluates them. */
static bool
recheck(ScanState* ss, TupleTableSlot* slot) {
    ChdbScanState* st     = (ChdbScanState*)ss;
    ExprContext* econtext = ss->ps.ps_ExprContext;

    econtext->ecxt_scantuple = slot;
    return ExecQualAndReset(st->recheck, econtext);
}

static TupleTableSlot*
exec_scan(CustomScanState* css) {
    return ExecScan(&css->ss, next_tuple, recheck);
}

void
chdb_planner_reset(ChdbScanState* st) {
    if (st->stream) {
        chdb_search_stream_close(st->stream);
        st->stream = NULL;
    }
    if (st->heap_slot) {
        ExecClearTuple(st->heap_slot); /* the last row's buffer pin */
    }
    MemoryContextReset(st->cxt); /* takes the hash table with it */
    st->seen         = NULL;
    st->sql          = NULL;
    st->built        = false;
    st->started      = false;
    st->skip         = false;
    st->asked        = -1;
    st->got          = 0;
    st->returned     = 0;
    st->heap_nblocks = 0;
}

static void
rescan(CustomScanState* css) {
    chdb_planner_reset((ChdbScanState*)css);
    ExecScanReScan(&css->ss);
}

static void
end_scan(CustomScanState* css) {
    ChdbScanState* st = (ChdbScanState*)css;

    chdb_planner_reset(st);
    if (st->fetch) {
        table_index_fetch_end(st->fetch);
    }
    index_close(st->index, NoLock);
    MemoryContextDelete(st->cxt);
}

const CustomScanMethods chdb_planner_scan_methods = {
    .CustomName            = "chdb_search",
    .CreateCustomScanState = create_scan_state,
};

const CustomExecMethods chdb_planner_exec_methods = {
    .CustomName        = "chdb_search",
    .BeginCustomScan   = begin_scan,
    .ExecCustomScan    = exec_scan,
    .EndCustomScan     = end_scan,
    .ReScanCustomScan  = rescan,
    .ExplainCustomScan = chdb_planner_explain,
};
