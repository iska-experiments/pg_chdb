#ifndef CHDB_SRF_H
#define CHDB_SRF_H

#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "nodes/execnodes.h"

/*
 * The contract every set-returning function of ours checks before it starts
 * chDB: the caller must take a materialized set and must say what columns it
 * wants. Returns the tuple descriptor of the caller's column definition list.
 * `prefix` opens each message and `fname` is the SQL name of the function.
 */
static inline TupleDesc
chdb_srf_tupdesc(FunctionCallInfo fcinfo, const char* prefix, const char* fname) {
    ReturnSetInfo* rsinfo = (ReturnSetInfo*)fcinfo->resultinfo;
    TupleDesc tupdesc;

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo)) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg(
                "%s: set-valued function called in context that cannot accept a set",
                prefix
            )
        );
    }
    if (!(rsinfo->allowedModes & SFRM_Materialize)) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg(
                "%s: materialize mode required, but it is not allowed in this context",
                prefix
            )
        );
    }
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("%s: a column definition list is required for %s", prefix, fname)
        );
    }

    return tupdesc;
}

#endif /* CHDB_SRF_H */
