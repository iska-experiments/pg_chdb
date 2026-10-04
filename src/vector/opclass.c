/*
 * The opclass support function: maps a distance strategy number to the
 * ClickHouse function the vector_similarity index is built with.
 */

#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"

#include "chdb_vector.h"

PG_FUNCTION_INFO_V1(chdb_vector_distance_name);
Datum
chdb_vector_distance_name(PG_FUNCTION_ARGS) {
    switch (PG_GETARG_INT16(0)) {
    case CHDB_VECTOR_STRATEGY_L2:
        PG_RETURN_TEXT_P(cstring_to_text(CHDB_VECTOR_FN_L2));
    case CHDB_VECTOR_STRATEGY_COSINE:
        PG_RETURN_TEXT_P(cstring_to_text(CHDB_VECTOR_FN_COSINE));
    case CHDB_VECTOR_STRATEGY_IP:
        PG_RETURN_TEXT_P(cstring_to_text(CHDB_VECTOR_FN_IP));
    default:
        ereport(
            ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unknown vector strategy number %d", PG_GETARG_INT16(0)))
        );
    }
    PG_RETURN_NULL();
}
