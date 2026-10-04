/*
 * Building the setup payload that chdb_helper reads from CHDB_SETUP_FD.
 */

#include "postgres.h"

#include "lib/stringinfo.h"

#include "helper.h"

static void
append_string(StringInfo buf, const char* str) {
    uint32_t len = (uint32_t)strlen(str);

    appendBinaryStringInfo(buf, (char*)&len, sizeof(len));
    appendBinaryStringInfo(buf, str, len);
}

/* The payload src/setup.h describes. */
void
chdb_helper_build_setup(
    StringInfo buf,
    chdbHelperContext* ctx,
    const char* query,
    char* const* names,
    char* const* values,
    size_t nparams
) {
    appendBinaryStringInfo(buf, (char*)&ctx->cmd, sizeof(ctx->cmd));
    appendBinaryStringInfo(buf, (char*)&ctx->max_memory, sizeof(ctx->max_memory));
    appendBinaryStringInfo(buf, (char*)&ctx->max_threads, sizeof(ctx->max_threads));
    appendBinaryStringInfo(buf, (char*)&ctx->max_parsers, sizeof(ctx->max_parsers));

    append_string(buf, query);

    uint16_t count = (uint16_t)nparams;
    appendBinaryStringInfo(buf, (char*)&count, sizeof(count));
    for (size_t i = 0; i < nparams; i++) {
        append_string(buf, names[i]);
        append_string(buf, values[i]);
    }

    if (buf->len > CHDB_SETUP_MAX) {
        ereport(
            ERROR,
            errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
            errmsg("chdb: query is too large to send to chDB")
        );
    }
}
