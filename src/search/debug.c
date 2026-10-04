/*
 * Superuser-only functions that exercise the worker from SQL, against the
 * fixed index OID 0 (chDB database idx_0). They exist so the worker can be
 * tested before the access method does; they are not an interface.
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"

/* Type mapping declarations; native.c is the TU carrying the implementation. */
#include "pg-clickhouse.h"

#include "../native.h"
#include "client.h"
#include "search.h"

#define CHDB_SEARCH_DEBUG_INDEX 0

static void
require_superuser(void) {
    if (!superuser()) {
        ereport(
            ERROR,
            errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
            errmsg("chdb_search: must be superuser to use the debug functions")
        );
    }
}

/* Runs a statement in idx_0. */
PG_FUNCTION_INFO_V1(chdb_search_debug_exec);
Datum
chdb_search_debug_exec(PG_FUNCTION_ARGS) {
    char* sql = text_to_cstring(PG_GETARG_TEXT_PP(0));

    require_superuser();
    chdbSearchConn* conn = chdb_search_connect();
    chdb_search_exec(conn, CHDB_SEARCH_DEBUG_INDEX, 0, sql);
    chdb_search_close(conn);

    PG_RETURN_VOID();
}

/* Drops idx_0 through the worker's DROP command. */
PG_FUNCTION_INFO_V1(chdb_search_debug_drop);
Datum
chdb_search_debug_drop(PG_FUNCTION_ARGS) {
    require_superuser();
    chdbSearchConn* conn = chdb_search_connect();
    chdb_search_drop(conn, CHDB_SEARCH_DEBUG_INDEX);
    chdb_search_close(conn);

    PG_RETURN_VOID();
}

/* Runs a query in idx_0, mapping its rows to the caller's column definition list. */
PG_FUNCTION_INFO_V1(chdb_search_debug_query);
Datum
chdb_search_debug_query(PG_FUNCTION_ARGS) {
    char* sql             = text_to_cstring(PG_GETARG_TEXT_PP(0));
    ReturnSetInfo* rsinfo = (ReturnSetInfo*)fcinfo->resultinfo;
    TupleDesc tupdesc;

    require_superuser();
    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
        !(rsinfo->allowedModes & SFRM_Materialize)) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg(
                "chdb_search: set-valued function called in context that cannot "
                "accept a set"
            )
        );
    }
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("chdb_search: a column definition list is required")
        );
    }

    chdbSearchConn* conn = chdb_search_connect();
    chdb_search_select(conn, CHDB_SEARCH_DEBUG_INDEX, 0, sql);
    Datum result = chdb_select_receive(sql, rsinfo, tupdesc, chdb_search_channel(conn));
    chdb_search_finish(conn);
    chdb_search_close(conn);

    return result;
}

/*
 * Streams the whole heap table into an INSERT statement for idx_0, such as
 * 'INSERT INTO idx_0.t (id, name)' into a table the caller made there with
 * chdb_search_exec (the scratch database has no generations), whose columns
 * are the table's. Returns the rows sent.
 */
PG_FUNCTION_INFO_V1(chdb_search_debug_copy_to);
Datum
chdb_search_debug_copy_to(PG_FUNCTION_ARGS) {
    Oid relid = PG_GETARG_OID(0);
    char* sql = text_to_cstring(PG_GETARG_TEXT_PP(1));

    require_superuser();
    Relation rel   = table_open(relid, AccessShareLock);
    TupleDesc desc = RelationGetDescr(rel);
    List* attnums  = NIL;

    for (int i = 0; i < desc->natts; i++) {
        Form_pg_attribute attr = TupleDescAttr(desc, i);

        if (!attr->attisdropped && !attr->attgenerated) {
            attnums = lappend_int(attnums, i + 1);
        }
    }

    chdbSearchConn* conn = chdb_search_connect();
    chdb_search_insert(conn, CHDB_SEARCH_DEBUG_INDEX, 0, sql);
    uint64_t rows = chdb_copy_send(
        rel, pgch_structure_from_tupdesc(desc, NULL), attnums, chdb_search_channel(conn)
    );
    chdb_search_finish(conn);
    chdb_search_close(conn);
    table_close(rel, AccessShareLock);

    PG_RETURN_INT64((int64)rows);
}

/*
 * The ClickHouse table holding a chdb index's rows, `idx_<oid>.t_<generation>`,
 * for reading the store through chdb_search_query.
 */
PG_FUNCTION_INFO_V1(chdb_search_debug_store_table);
Datum
chdb_search_debug_store_table(PG_FUNCTION_ARGS) {
    Oid indexoid = PG_GETARG_OID(0);

    require_superuser();
    Relation index = index_open(indexoid, AccessShareLock);
    char* table    = chdb_search_table_name(index);

    index_close(index, AccessShareLock);

    PG_RETURN_TEXT_P(cstring_to_text(table));
}

/*
 * The metapage of a chdb index: the magic, version, generation and WAL
 * position of the last flush (meta.c), as a row of the function's result type.
 */
PG_FUNCTION_INFO_V1(chdb_search_debug_metapage);
Datum
chdb_search_debug_metapage(PG_FUNCTION_ARGS) {
    Oid indexoid = PG_GETARG_OID(0);
    TupleDesc desc;
    ChdbMetaPageData meta;
    Datum values[4];
    bool nulls[4] = { false, false, false, false };

    require_superuser();
    if (!chdb_search_is_index(indexoid)) {
        ereport(
            ERROR,
            errcode(ERRCODE_WRONG_OBJECT_TYPE),
            errmsg("\"%s\" is not a chdb index", get_rel_name(indexoid))
        );
    }
    if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE) {
        elog(ERROR, "chdb_search: the metapage function must return a row");
    }

    Relation index = index_open(indexoid, AccessShareLock);

    chdb_meta_read(index, &meta);
    index_close(index, AccessShareLock);
    values[0] = Int64GetDatum((int64)meta.magic);
    values[1] = Int32GetDatum((int32)meta.version);
    values[2] = CStringGetTextDatum(psprintf(UINT64_FORMAT, meta.generation));
    values[3] = LSNGetDatum((XLogRecPtr)meta.flushed_lsn);

    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, values, nulls)));
}

/* The pid of the worker's engine process, or NULL when none runs yet. */
PG_FUNCTION_INFO_V1(chdb_search_debug_engine_pid);
Datum
chdb_search_debug_engine_pid(PG_FUNCTION_ARGS) {
    require_superuser();
    chdbSearchConn* conn = chdb_search_connect();
    int pid              = chdb_search_engine_pid(conn);

    chdb_search_close(conn);
    if (pid == 0) {
        PG_RETURN_NULL();
    }

    PG_RETURN_INT32(pid);
}

/*
 * Signals the engine, as a crash would. Does not wait for it to die: the next
 * request finds out and reports it. Returns the pid signalled.
 */
PG_FUNCTION_INFO_V1(chdb_search_debug_kill_engine);
Datum
chdb_search_debug_kill_engine(PG_FUNCTION_ARGS) {
    int signo = PG_GETARG_INT32(0);

    require_superuser();
    chdbSearchConn* conn = chdb_search_connect();
    int pid              = chdb_search_engine_kill(conn, signo);

    chdb_search_close(conn);

    PG_RETURN_INT32(pid);
}
