/*
 * The multi-insert buffer and the column defaults of a COPY from chDB, as
 * copyfrom.c keeps them: the rows native_recv.c decodes wait here for a
 * table_multi_insert when the target allows one, and go in one at a time
 * when it does not. See native_insert.h.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/tableam.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "optimizer/optimizer.h"
#include "rewrite/rewriteHandler.h"
#include "utils/rel.h"

#include "native_insert.h"

/* Bytes to buffer before a table_multi_insert, as copyfrom.c does. */
#define CHDB_MAX_BUFFERED_BYTES (64 * 1024)

/* ExecInsertIndexTuples' argument order changed in PG 19. */
static inline List*
insert_index_tuples(ResultRelInfo* rri, TupleTableSlot* slot, EState* estate) {
#if PG_VERSION_NUM >= 190000
    return ExecInsertIndexTuples(rri, estate, 0, slot, NIL, NULL);
#elif PG_VERSION_NUM >= 160000
    return ExecInsertIndexTuples(rri, slot, estate, false, false, NULL, NIL, false);
#else
    return ExecInsertIndexTuples(rri, slot, estate, false, false, NULL, NIL);
#endif
}

/* Index entries and AFTER ROW triggers for a tuple already in the table. */
static void
after_insert(chdbNativeInsert* ins, ResultRelInfo* rri, TupleTableSlot* slot) {
    List* recheck =
        rri->ri_NumIndices > 0 ? insert_index_tuples(rri, slot, ins->estate) : NIL;

    ExecARInsertTriggers(ins->estate, rri, slot, recheck, ins->transition);
    list_free(recheck);
}

void
chdb_native_insert_flush(chdbNativeInsert* ins) {
    if (!ins->nused) {
        return;
    }

    /* table_multi_insert may leak, so give it a context that gets reset. */
    MemoryContext oldcxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(ins->estate));

    table_multi_insert(
        ins->target->ri_RelationDesc,
        ins->slots,
        ins->nused,
        ins->cid,
        ins->ti_options,
        ins->bistate
    );
    MemoryContextSwitchTo(oldcxt);

    for (int i = 0; i < ins->nused; i++) {
        after_insert(ins, ins->target, ins->slots[i]);
        ExecClearTuple(ins->slots[i]);
    }

    ins->nused = 0;
    ins->bytes = 0;
}

TupleTableSlot*
chdb_native_insert_slot(chdbNativeInsert* ins) {
    if (!ins->slots[ins->nused]) {
        ins->slots[ins->nused] = table_slot_create(ins->target->ri_RelationDesc, NULL);
    }

    return ins->slots[ins->nused];
}

void
chdb_native_insert_store(chdbNativeInsert* ins, TupleTableSlot* slot) {
    ins->bytes += heap_compute_data_size(
        slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull
    );

    /* The values point into the per-row context, so the slot needs its own. */
    ExecMaterializeSlot(slot);
    ins->nused++;

    if (ins->nused >= CHDB_MAX_BUFFERED_TUPLES ||
        ins->bytes >= CHDB_MAX_BUFFERED_BYTES) {
        chdb_native_insert_flush(ins);
    }
}

bool
chdb_native_insert_row(
    chdbNativeInsert* ins,
    ResultRelInfo* rri,
    TupleTableSlot* slot
) {
    if (rri->ri_FdwRoutine) {
        slot = rri->ri_FdwRoutine->ExecForeignInsert(ins->estate, rri, slot, NULL);
        if (!slot) {
            return false; /* "do nothing" */
        }

        /* AFTER ROW triggers might reference the tableoid column. */
        slot->tts_tableOid = RelationGetRelid(rri->ri_RelationDesc);
        ExecARInsertTriggers(ins->estate, rri, slot, NIL, ins->transition);
        return true;
    }

    table_tuple_insert(
        rri->ri_RelationDesc, slot, ins->cid, ins->ti_options, ins->bistate
    );
    after_insert(ins, rri, slot);

    return true;
}

chdbNativeDefaults
chdb_native_defaults_for(Relation rel, List* attnums) {
    TupleDesc desc              = RelationGetDescr(rel);
    chdbNativeDefaults defaults = { .n     = 0,
                                    .dest  = palloc(desc->natts * sizeof(int)),
                                    .exprs = palloc(desc->natts * sizeof(ExprState*)) };

    for (int attnum = 1; attnum <= desc->natts; attnum++) {
        Form_pg_attribute attr = TupleDescAttr(desc, attnum - 1);

        /* ExecComputeStoredGenerated computes a generated column instead. */
        if (attr->attisdropped || attr->attgenerated ||
            list_member_int(attnums, attnum)) {
            continue;
        }
        Expr* expr = (Expr*)build_column_default(rel, attnum);
        if (!expr) {
            continue;
        }

        defaults.dest[defaults.n]    = attnum - 1;
        defaults.exprs[defaults.n++] = ExecInitExpr(expression_planner(expr), NULL);
    }

    return defaults;
}

void
chdb_native_defaults_fill(
    const chdbNativeDefaults* defaults,
    EState* estate,
    TupleTableSlot* slot
) {
    if (!defaults->n) {
        return;
    }

    ExprContext* econtext = GetPerTupleExprContext(estate);
    MemoryContext oldcxt  = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
    for (int i = 0; i < defaults->n; i++) {
        slot->tts_values[defaults->dest[i]] = ExecEvalExpr(
            defaults->exprs[i], econtext, &slot->tts_isnull[defaults->dest[i]]
        );
    }
    MemoryContextSwitchTo(oldcxt);
}
