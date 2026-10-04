/*
 * The columns a DESCRIBE reports, read from the Native blocks a chDB child
 * streams: a name and a type per row, with describe_compact_output. CREATE
 * TABLE from a URL derives its columns from them (hook/create.c).
 */

#include "postgres.h"

#include "utils/builtins.h"
#include "utils/memutils.h"

#include "pg-clickhouse-decode.h"

#include "native.h"

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
        chdb_native_reader_error(reader.error);
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
        chdb_native_reader_error(reader.error);
    }

    MemoryContextSwitchTo(oldcxt);
    MemoryContextDelete(streamcxt);

    return columns;
}
