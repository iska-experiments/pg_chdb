/*
 * The chdb_vector.* GUCs and the ClickHouse SETTINGS fragment built from them.
 */

#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "chdb_vector.h"

enum { FILTER_AUTO, FILTER_POSTFILTER, FILTER_PREFILTER };

static const struct config_enum_entry filter_strategies[] = {
    { "auto",       FILTER_AUTO,       false },
    { "postfilter", FILTER_POSTFILTER, false },
    { "prefilter",  FILTER_PREFILTER,  false },
    { NULL,         0,                 false },
};

static int vector_candidates = 256;
static bool vector_rescoring = false;
static int vector_filter     = FILTER_AUTO;

void
chdb_vector_define_gucs(void) {
    DefineCustomIntVariable(
        CHDB_VECTOR_GUC_CANDIDATES,
        "Candidates examined per HNSW search.",
        "Sent as hnsw_candidate_list_size_for_search; higher is slower and more "
        "accurate.",
        &vector_candidates,
        256,
        1,
        100000,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    DefineCustomBoolVariable(
        CHDB_VECTOR_GUC_RESCORING,
        "Rescore HNSW candidates with exact distances.",
        "Sent as vector_search_with_rescoring.",
        &vector_rescoring,
        false,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    DefineCustomEnumVariable(
        CHDB_VECTOR_GUC_FILTER,
        "How filters combine with HNSW search.",
        "Sent as vector_search_filter_strategy.",
        &vector_filter,
        FILTER_AUTO,
        filter_strategies,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    MarkGUCPrefixReserved("chdb_vector");
}

PG_FUNCTION_INFO_V1(chdb_vector_query_settings);
Datum
chdb_vector_query_settings(PG_FUNCTION_ARGS) {
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfo(
        &buf,
        "SETTINGS %s = %d, %s = %d, %s = '%s'",
        CHDB_VECTOR_SETTING_CANDIDATES,
        vector_candidates,
        CHDB_VECTOR_SETTING_RESCORING,
        vector_rescoring ? 1 : 0,
        CHDB_VECTOR_SETTING_FILTER,
        vector_filter == FILTER_AUTO         ? "auto"
        : vector_filter == FILTER_POSTFILTER ? "postfilter"
                                             : "prefilter"
    );
    PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}
