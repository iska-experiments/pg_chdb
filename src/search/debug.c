/*
 * Superuser-only functions that exercise the worker from SQL, against the
 * fixed index OID 0 (chDB database idx_0). They exist so the worker can be
 * tested before the access method does; they are not an interface.
 */

#include "postgres.h"

#include "access/table.h"
#include "catalog/pg_class.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/rel.h"

/* Type mapping declarations; native.c is the TU carrying the implementation. */
#include "pg-clickhouse.h"

#include "../native.h"
#include "client.h"

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
    chdb_search_exec(conn, CHDB_SEARCH_DEBUG_INDEX, sql);
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
    chdb_search_select(conn, CHDB_SEARCH_DEBUG_INDEX, sql);
    Datum result = chdb_select_receive(sql, rsinfo, tupdesc, chdb_search_channel(conn));
    chdb_search_finish(conn);
    chdb_search_close(conn);

    return result;
}

/*
 * Streams the whole heap table into an INSERT statement for idx_0, such as
 * 'INSERT INTO idx_0.t (id, name)', whose columns are the table's. Returns
 * the rows sent.
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
    chdb_search_insert(conn, CHDB_SEARCH_DEBUG_INDEX, sql);
    uint64_t rows = chdb_copy_send(
        rel, pgch_structure_from_tupdesc(desc, NULL), attnums, chdb_search_channel(conn)
    );
    chdb_search_finish(conn);
    chdb_search_close(conn);
    table_close(rel, AccessShareLock);

    PG_RETURN_INT64((int64)rows);
}
