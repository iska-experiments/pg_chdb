/*
 * The rows of a chDB query, materialized for a set-returning function: the
 * Native blocks a chDB child streams, decoded into the Postgres types of the
 * caller's column definition list and put in a tuplestore for the executor
 * to hand on. chdb_query and chdb_search_query both end here.
 */

#include "postgres.h"

#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"

#include "pg-clickhouse-decode.h"

#include "native.h"

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
        chdb_native_reader_error(reader.error);
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
