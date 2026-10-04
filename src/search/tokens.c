/*
 * chdb.tokens(text): what the engine's tokens() makes of a string, through
 * the worker, for seeing how a column will be tokenized. The function is
 * tied to no index, so the request names none.
 */

#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/memutils.h"

#include "../native.h"
#include "pg-clickhouse-decode.h"
#include "query.h"
#include "search.h"

PG_FUNCTION_INFO_V1(chdb_search_tokens);
Datum
chdb_search_tokens(PG_FUNCTION_ARGS) {
    StringInfoData sql;
    MemoryContext cxt = AllocSetContextCreate(
        CurrentMemoryContext, "chdb_search tokens", ALLOCSET_SMALL_SIZES
    );
    Datum result = (Datum)0;
    bool found   = false;

    initStringInfo(&sql);
    appendStringInfoString(&sql, "SELECT tokens(");
    chdb_search_append_string(&sql, text_to_cstring(PG_GETARG_TEXT_PP(0)));
    appendStringInfoChar(&sql, ')');

    chdbSearchConn* conn = chdb_search_connect();
    MemoryContext old    = MemoryContextSwitchTo(cxt);

    PG_TRY();
    {
        pgch_reader reader;
        pgch_block_source src;

        chdb_search_log_sql("select", sql.data);
        chdb_search_select(conn, InvalidOid, 0, sql.data);
        src = chdb_native_source(chdb_search_channel(conn));
        pgch_reader_init(&reader, &src);
        if (reader.error) {
            ereport(
                ERROR,
                errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                errmsg("chdb_search: %s", reader.error)
            );
        }
        if (pgch_reader_columns(&reader) == 1) {
            void* state = pgch_reader_convert_init(&reader, 0, TEXTARRAYOID, -1);
            Datum v;
            bool n;

            if (pgch_reader_next(&reader)) {
                pgch_reader_fill(&reader, &state, &v, &n);
                MemoryContextSwitchTo(old);
                result = n ? (Datum)0 : datumCopy(v, false, -1);
                found  = !n;
                MemoryContextSwitchTo(cxt);
            }
            if (reader.error) {
                ereport(
                    ERROR,
                    errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                    errmsg("chdb_search: %s", reader.error)
                );
            }
        }
        chdb_search_finish(conn);
    }
    PG_FINALLY();
    {
        MemoryContextSwitchTo(old);
        chdb_search_close(conn);
    }
    PG_END_TRY();
    MemoryContextDelete(cxt);

    if (!found) {
        PG_RETURN_ARRAYTYPE_P(construct_empty_array(TEXTOID));
    }
    PG_RETURN_DATUM(result);
}
