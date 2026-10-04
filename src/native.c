/*
 * COPY from chDB to Postgres in ClickHouse's Native format, and the one TU
 * defining the clickhouse-c and pg-clickhouse-c implementations, both halves:
 * the encoder's header stays included for that though native_send.c is what
 * encodes.
 *
 * Values cross as Datums in both directions: a pgch_writer fed from scan slots
 * on the way out (native_send.c), a pgch_reader over the helper's output
 * feeding an insert loop on the way in. Nothing passes through COPY's text
 * escaping, so arrays, decimals and timestamps keep their types instead of
 * collapsing to String.
 *
 * The insert loop mirrors CopyFrom in src/backend/commands/copyfrom.c, which
 * cannot be reused because its row source is its own text parser: triggers,
 * generated columns, constraints, partition routing, index maintenance and
 * multi-insert buffering are all replayed here.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 */

#include "postgres.h"

#include <string.h>

#include "access/heapam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/tupconvert.h"
#include "access/xact.h"
#include "catalog/pg_class.h"
#include "commands/trigger.h"
#include "executor/execPartition.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"

#define CHC_IMPLEMENTATION
#define PGCH_IMPLEMENTATION
#include "clickhouse.h"

#include "pg-clickhouse-decode.h"
#include "pg-clickhouse-encode.h"

#include "native.h"
#include "native_insert.h"

/* ---- chDB to Postgres ------------------------------------------------ */

/*
 * Buffer small reads from helper. Read large chunks directly into destination,
 * so only block metadata usually passes through this buffer.
 */
#define CHDB_NATIVE_SOURCE_BYTES (256 * 1024)

/* Reads Native output from helper one block at a time. */
typedef struct nativeSource {
    chc_io io;
    chc_in* in;
    MemoryContext cxt; /* decoded blocks outlive rows read from them */
    chdbChannel* helper;
    char* error;
} nativeSource;

/* Helper failures are reported directly, so callback always returns success. */
static int
source_read(
    void* ud,
    void* buf,
    size_t len,
    size_t* got,
    chc_err* err pg_attribute_unused()
) {
    nativeSource* s = ud;

    *got = chdb_channel_recv(s->helper, buf, len);

    return CHC_OK;
}

/* Checks for interrupts before decoder refills input buffer. */
static int
source_cancelled(void* ud pg_attribute_unused()) {
    CHECK_FOR_INTERRUPTS();

    return 0;
}

static const chc_block*
source_next(void* ud) {
    nativeSource* s = ud;
    chc_block* block;
    chc_err err = {};

    if (s->error) {
        return NULL;
    }

    MemoryContext oldcxt = MemoryContextSwitchTo(s->cxt);

    /* NULL block without an error marks end of stream. */
    if (chc_block_read(s->in, &pgch_alloc, &pgch_block_opts_local, &block, &err) !=
        CHC_OK) {
        s->error = MemoryContextStrdup(
            s->cxt, err.msg[0] ? err.msg : "chDB block could not be read"
        );
        block = NULL;
    }
    MemoryContextSwitchTo(oldcxt);

    return block;
}

static const char*
source_error(void* ud) {
    return ((nativeSource*)ud)->error;
}

pgch_block_source
chdb_native_source(chdbChannel* helper) {
    nativeSource* s = palloc0(sizeof(*s));
    chc_err err     = {};

    s->helper = helper;
    s->cxt    = CurrentMemoryContext;
    s->io = (chc_io){ .ud = s, .read = source_read, .check_cancel = source_cancelled };
    s->in = pgch_in_alloc();
    if (chc_in_init(s->in, &s->io, &pgch_alloc, CHDB_NATIVE_SOURCE_BYTES, &err) !=
        CHC_OK) {
        pgch_raise(&err, ERRCODE_EXTERNAL_ROUTINE_EXCEPTION, "reader init: ", NULL);
    }

    return (pgch_block_source){ .ud         = s,
                                .next_block = source_next,
                                .error      = source_error };
}

/* Reader errors carry the chDB message; the query text is the caller's. */
pg_noreturn static void
report_reader_error(const char* error) {
    ereport(
        ERROR,
        errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
        errmsg("chdb: error fetching chDB query result"),
        errdetail("%s", error)
    );
}

/*
 * Read one source column from each DESCRIBE row
 * describe_compact_output limits result to String name and type columns
 */
List*
chdb_native_describe(chdbChannel* helper) {
    /* Keep reader buffers temporary and allocate returned columns in caller context */
    MemoryContext streamcxt = AllocSetContextCreate(
        CurrentMemoryContext, "chdb describe", ALLOCSET_SMALL_SIZES
    );
    MemoryContext oldcxt  = MemoryContextSwitchTo(streamcxt);
    pgch_block_source src = chdb_native_source(helper);
    List* columns         = NIL;
    pgch_reader reader;

    pgch_reader_init(&reader, &src);
    if (reader.error) {
        report_reader_error(reader.error);
    }
    if (pgch_reader_columns(&reader) != 2) {
        ereport(
            ERROR,
            errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
            errmsg(
                "chdb: DESCRIBE returned %zu columns, expected 2",
                pgch_reader_columns(&reader)
            )
        );
    }

    while (pgch_reader_next(&reader)) {
        Datum values[2];
        bool nulls[2];

        pgch_reader_fill(&reader, NULL, values, nulls);
        if (nulls[0] || nulls[1]) {
            ereport(
                ERROR,
                errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
                errmsg("chdb: DESCRIBE produced a column with no name or type")
            );
        }

        MemoryContextSwitchTo(oldcxt);
        chdbDescribedColumn* col = palloc(sizeof(*col));

        col->name = TextDatumGetCString(values[0]);
        col->type = TextDatumGetCString(values[1]);
        columns   = lappend(columns, col);
        MemoryContextSwitchTo(streamcxt);
    }
    if (reader.error) {
        report_reader_error(reader.error);
    }

    MemoryContextSwitchTo(oldcxt);
    MemoryContextDelete(streamcxt);

    return columns;
}

/*
 * CopyFrom from src/backend/commands/copyfrom.c, NextCopyFrom replaced by
 * pgch_reader_next. The steps between the row source and table_tuple_insert are
 * that function's, in its order, so diff against it when a release moves the
 * insert path. Reader setup comes first, then markers bound the copied part.
 *
 * Not carried over: FREEZE and the new-in-transaction ti_options, the WHERE
 * filter, and on_error's soft-error retry. Defaults are evaluated per row, so
 * copyfrom.c's volatile_defexprs test has no analogue below.
 */
uint64_t
chdb_copy_receive(
    Relation rel,
    List* attnums,
    List* rtable,
    List* rteperminfos,
    uint16_t encoding_check,
    chdbChannel* helper
) {
    TupleDesc desc = RelationGetDescr(rel);
    /* Blocks and reader state live here, one row's values in rowcxt. */
    MemoryContext streamcxt = AllocSetContextCreate(
        CurrentMemoryContext, "chdb stream", ALLOCSET_DEFAULT_SIZES
    );
    MemoryContext rowcxt =
        AllocSetContextCreate(CurrentMemoryContext, "chdb row", ALLOCSET_DEFAULT_SIZES);
    MemoryContext oldcxt = MemoryContextSwitchTo(streamcxt);

    pgch_block_source src = chdb_native_source(helper);
    size_t ncols          = list_length(attnums);
    int* dest             = palloc(ncols * sizeof(int));
    size_t n              = 0;

    ListCell* lc;
    foreach (lc, attnums) {
        dest[n++] = lfirst_int(lc) - 1;
    }

    pgch_reader reader;
    pgch_reader_init(&reader, &src);
    if (reader.error) {
        report_reader_error(reader.error);
    }
    reader.encoding_check = encoding_check;
    if (pgch_reader_columns(&reader) == 0) {
        /* Nothing streamed at all, so there is no schema to check. */
        MemoryContextSwitchTo(oldcxt);
        MemoryContextDelete(streamcxt);
        MemoryContextDelete(rowcxt);
        return 0;
    }
    if (pgch_reader_columns(&reader) != ncols) {
        ereport(
            ERROR,
            errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
            errmsg(
                "chdb: chDB returned %zu columns, expected %zu",
                pgch_reader_columns(&reader),
                ncols
            )
        );
    }

    /* Conversion state per column, off the column type rather than a value. */
    void** states = palloc0(ncols * sizeof(void*));

    for (size_t i = 0; i < ncols; i++) {
        Form_pg_attribute attr = TupleDescAttr(desc, dest[i]);
        states[i] =
            pgch_reader_convert_init(&reader, i, attr->atttypid, attr->atttypmod);
    }

    MemoryContextSwitchTo(oldcxt);

    chdbNativeDefaults defaults = chdb_native_defaults_for(rel, attnums);

    /* ---- from here on, copyfrom.c's CopyFrom ---- */

    /*
     * The executor wants a range table to make index entries against. It is
     * the one hook.c already built for `rel` and checked the copied columns
     * against, so the entry the insert runs under is the entry that passed.
     */
    EState* estate = CreateExecutorState();

#if PG_VERSION_NUM >= 180000
    ExecInitRangeTable(estate, rtable, rteperminfos, bms_make_singleton(1));
#elif PG_VERSION_NUM >= 160000
    ExecInitRangeTable(estate, rtable, rteperminfos);
#else
    ExecInitRangeTable(estate, rtable);
#endif
    ResultRelInfo* target = makeNode(ResultRelInfo);

    ExecInitResultRelation(estate, target, 1);
#if PG_VERSION_NUM >= 180000
    CheckValidResultRel(target, CMD_INSERT, ONCONFLICT_NONE, NIL);
#elif PG_VERSION_NUM >= 170000
    CheckValidResultRel(target, CMD_INSERT, NIL);
#else
    CheckValidResultRel(target, CMD_INSERT);
#endif
    ExecOpenIndices(target, false);

    /* A foreign table target initializes itself through a ModifyTableState. */
    ModifyTableState* mtstate = makeNode(ModifyTableState);

    mtstate->ps.plan           = NULL;
    mtstate->ps.state          = estate;
    mtstate->operation         = CMD_INSERT;
    mtstate->mt_nrels          = 1;
    mtstate->resultRelInfo     = target;
    mtstate->rootResultRelInfo = target;
    if (target->ri_FdwRoutine && target->ri_FdwRoutine->BeginForeignInsert) {
        target->ri_FdwRoutine->BeginForeignInsert(mtstate, target);
    }
    target->ri_BatchSize = 1;

    AfterTriggerBeginQuery();

    /* Partition routing wants to know whether transition tuples are captured. */
    chdbNativeInsert ins = {};

    ins.transition = mtstate->mt_transition_capture =
        MakeTransitionCaptureState(rel->trigdesc, RelationGetRelid(rel), CMD_INSERT);

    PartitionTupleRouting* proute = NULL;

    if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE) {
        proute = ExecSetupPartitionTupleRouting(estate, rel);
    }

    bool before_row =
        target->ri_TrigDesc && target->ri_TrigDesc->trig_insert_before_row;
    bool instead_row =
        target->ri_TrigDesc && target->ri_TrigDesc->trig_insert_instead_row;

    /*
     * BEFORE and INSTEAD OF triggers may query the table, so rows they see
     * cannot sit in a buffer. Neither can a routed row: a buffer belongs to
     * one relation, and copyfrom.c's per-partition buffers are more machinery
     * than a first cut needs.
     */
    ins.estate     = estate;
    ins.target     = target;
    ins.cid        = GetCurrentCommandId(true);
    ins.ti_options = 0;
    ins.bistate    = GetBulkInsertState();
    ins.buffered   = !proute && !target->ri_FdwRoutine && !before_row && !instead_row;

    TupleTableSlot* rootslot = table_slot_create(rel, &estate->es_tupleTable);
    ResultRelInfo* routed    = NULL;
    uint64_t rows            = 0;

    ExecBSInsertTriggers(estate, target);

    for (;;) {
        CHECK_FOR_INTERRUPTS();
        ResetPerTupleExprContext(estate);
        MemoryContextReset(rowcxt);

        TupleTableSlot* slot = ins.buffered ? chdb_native_insert_slot(&ins) : rootslot;
        ExecClearTuple(slot);
        /* Attributes without a stream column, default or generator stay null. */
        memset(slot->tts_isnull, true, desc->natts * sizeof(bool));

        /*
         * NextCopyFrom's place in CopyFrom. Values decode into rowcxt, but the
         * call crossing into the next block allocates that block too, and a
         * block outlives the row that pulled it in. Run that one call in
         * streamcxt and leave its row's values there: one row per block, not
         * one per row.
         */
        bool crossing = !reader.cur || reader.row >= chc_block_n_rows(reader.cur);

        MemoryContextSwitchTo(crossing ? streamcxt : rowcxt);
        if (!pgch_reader_next(&reader)) {
            MemoryContextSwitchTo(oldcxt);
            break;
        }
        pgch_reader_fill_map(&reader, states, dest, slot->tts_values, slot->tts_isnull);
        MemoryContextSwitchTo(oldcxt);
        chdb_native_defaults_fill(&defaults, estate, slot);
        ExecStoreVirtualTuple(slot);

        /* Constraints may reference the tableoid column. */
        slot->tts_tableOid = RelationGetRelid(rel);

        ResultRelInfo* rri = target;
        if (proute) {
            /* Raises when no partition of the row's key exists. */
            rri = ExecFindPartition(mtstate, target, proute, slot, estate);
            if (rri != routed) {
                before_row =
                    rri->ri_TrigDesc && rri->ri_TrigDesc->trig_insert_before_row;
                instead_row =
                    rri->ri_TrigDesc && rri->ri_TrigDesc->trig_insert_instead_row;
                ReleaseBulkInsertStatePin(ins.bistate);
                routed = rri;
            }

            /*
             * A BEFORE trigger on the partition can change the tuple, so only
             * an untriggered partition can hand its row to transition capture
             * unconverted.
             */
            if (ins.transition) {
                ins.transition->tcs_original_insert_tuple = before_row ? NULL : slot;
            }

#if PG_VERSION_NUM >= 160000
            TupleConversionMap* map = ExecGetRootToChildMap(rri, estate);
#else
            TupleConversionMap* map = rri->ri_RootToPartitionMap;
#endif
            if (map) {
                slot = execute_attr_map_slot(
                    map->attrMap, slot, rri->ri_PartitionTupleSlot
                );
            }
            slot->tts_tableOid = RelationGetRelid(rri->ri_RelationDesc);
        }

        if (before_row && !ExecBRInsertTriggers(estate, rri, slot)) {
            continue; /* "do nothing" */
        }

        if (instead_row) {
            ExecIRInsertTriggers(estate, rri, slot);
        } else {
            if (rri->ri_RelationDesc->rd_att->constr &&
                rri->ri_RelationDesc->rd_att->constr->has_generated_stored) {
                ExecComputeStoredGenerated(rri, estate, slot, CMD_INSERT);
            }
            if (!rri->ri_FdwRoutine && rri->ri_RelationDesc->rd_att->constr) {
                ExecConstraints(rri, slot, estate);
            }

            /*
             * Routing already proved the partition constraint, unless a BEFORE
             * trigger has had the tuple since.
             */
            if (rri->ri_RelationDesc->rd_rel->relispartition &&
                (!proute || before_row)) {
                ExecPartitionCheck(rri, slot, estate, true);
            }

            if (ins.buffered) {
                chdb_native_insert_store(&ins, slot);
            } else if (!chdb_native_insert_row(&ins, rri, slot)) {
                continue;
            }
        }

        rows++;
    }

    /* Ours: the loop ends on a reader error the same way it ends on no rows. */
    if (reader.error) {
        report_reader_error(reader.error);
    }

    chdb_native_insert_flush(&ins);
    for (int i = 0; i < CHDB_MAX_BUFFERED_TUPLES && ins.slots[i]; i++) {
        ExecDropSingleTupleTableSlot(ins.slots[i]);
    }
    FreeBulkInsertState(ins.bistate);
    if (ins.buffered) {
        table_finish_bulk_insert(rel, ins.ti_options);
    }

    ExecASInsertTriggers(estate, target, ins.transition);
    AfterTriggerEndQuery(estate);
    ExecResetTupleTable(estate->es_tupleTable, false);

    if (target->ri_FdwRoutine && target->ri_FdwRoutine->EndForeignInsert) {
        target->ri_FdwRoutine->EndForeignInsert(estate, target);
    }
    if (proute) {
        ExecCleanupTupleRouting(mtstate, proute);
    }

    /* Closes the indices ExecOpenIndices opened. */
    ExecCloseResultRelations(estate);
    ExecCloseRangeTableRelations(estate);
    FreeExecutorState(estate);

    /* ---- end of CopyFrom ---- */

    MemoryContextDelete(streamcxt);
    MemoryContextDelete(rowcxt);

    return rows;
}

/*
 * Execute a query against a temporary chDB database and return its rows,
 * mapping chDB values to the Postgres types named in the caller's column
 * definition list and streaming the results back to the client.
 */
Datum
chdb_select_receive(
    char* query,
    ReturnSetInfo* rsinfo,
    TupleDesc tupdesc,
    chdbChannel* helper
) {
    /*
     * Build result info in per-query context so it outlives this call.
     */
    MemoryContext query_ctx   = rsinfo->econtext->ecxt_per_query_memory;
    MemoryContext old_ctx     = MemoryContextSwitchTo(query_ctx);
    tupdesc                   = CreateTupleDescCopy(tupdesc);
    Tuplestorestate* tupstore = tuplestore_begin_heap(
        rsinfo->allowedModes & SFRM_Materialize_Random, false, work_mem
    );

    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult  = tupstore;
    rsinfo->setDesc    = tupdesc;

    MemoryContextSwitchTo(old_ctx);

    /* Set up row destination and the query reader. */
    Datum* values = palloc(tupdesc->natts * sizeof(Datum));
    bool* nulls   = palloc0(tupdesc->natts * sizeof(bool));

    pgch_block_source src = chdb_native_source(helper);
    pgch_reader reader;
    pgch_reader_init(&reader, &src);
    if (reader.error) {
        report_reader_error(reader.error);
    }

    /* Per-row values are copied into tuplestore; reset between rows. */
    MemoryContext row_cxt = AllocSetContextCreate(
        CurrentMemoryContext, "chdb_query row", ALLOCSET_DEFAULT_SIZES
    );

    /*
     * Fetch the columns from the chDB query. Use row context because it
     * fetches the first block to get the columns.
     */
    MemoryContextSwitchTo(row_cxt);
    if (pgch_reader_columns(&reader) == 0) {
        /* Nothing streamed at all, so there is no schema to check. */
        MemoryContextSwitchTo(old_ctx);
        MemoryContextDelete(row_cxt);
        return (Datum)0;
    }
    if (pgch_reader_columns(&reader) != tupdesc->natts) {
        ereport(
            ERROR,
            errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
            errmsg(
                "chdb: chDB returned %zu columns, expected %d",
                pgch_reader_columns(&reader),
                tupdesc->natts
            )
        );
    }

    /*
     * Populate the conversion state per column, from the column type, not the
     * value. Configure the Postgres destination column attnum per chDB column
     * so tuplestore_putvalues() below knows where to put things.
     */
    MemoryContextSwitchTo(query_ctx);
    void** states  = palloc0(tupdesc->natts * sizeof(void*));
    int* attr_nums = palloc(tupdesc->natts * sizeof(int));
    for (size_t i = 0; i < tupdesc->natts; i++) {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        attr_nums[i]           = attr->attnum - 1;
        states[i] =
            pgch_reader_convert_init(&reader, i, attr->atttypid, attr->atttypmod);
    }

    /* Fetch the data from chDB. */
    for (;;) {
        MemoryContextSwitchTo(row_cxt);
        if (!pgch_reader_next(&reader)) {
            /* No more rows to process. */
            MemoryContextSwitchTo(old_ctx);
            break;
        }

        /*
         * Use states to convert values from reader and store in values &
         * nulls in positions defined by attr_nums.
         */
        pgch_reader_fill_map(&reader, states, attr_nums, values, nulls);

        /* Send the resulting Datums in the tuple store for Postgres to process. */
        tuplestore_putvalues(tupstore, tupdesc, values, nulls);

        MemoryContextReset(row_cxt);
        CHECK_FOR_INTERRUPTS();
    }

    /* Clean up and return. */
    MemoryContextDelete(row_cxt);
    return (Datum)0;
}
