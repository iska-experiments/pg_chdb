/*
 * Framing a request of protocol.h: command, index OID and chDB settings, then
 * the query and an empty parameter list. client.c sends the frame to the
 * worker over its socket; sweep.c sends it from the worker to its engine.
 */

#include "postgres.h"

#include "frame.h"
#include "worker.h"

static void
append_string(StringInfo buf, const char* str) {
    uint32_t len = (uint32_t)strlen(str);

    appendBinaryStringInfo(buf, (char*)&len, sizeof(len));
    appendBinaryStringInfo(buf, str, len);
}

void
chdb_search_frame_request(
    StringInfo buf,
    chdbCmdType cmd,
    Oid index,
    uint64 generation,
    const char* sql
) {
    uint16_t memory  = (uint16_t)chdb_max_memory;
    uint16_t threads = (uint16_t)chdb_max_threads;
    uint16_t parsers = (uint16_t)chdb_max_parsers;
    uint16_t nparams = 0;

    appendBinaryStringInfo(buf, (char*)&cmd, sizeof(cmd));
    appendBinaryStringInfo(buf, (char*)&index, sizeof(index));
    appendBinaryStringInfo(buf, (char*)&generation, sizeof(generation));
    appendBinaryStringInfo(buf, (char*)&memory, sizeof(memory));
    appendBinaryStringInfo(buf, (char*)&threads, sizeof(threads));
    appendBinaryStringInfo(buf, (char*)&parsers, sizeof(parsers));
    append_string(buf, sql);
    appendBinaryStringInfo(buf, (char*)&nparams, sizeof(nparams));

    if (buf->len > CHDB_SETUP_MAX) {
        ereport(
            ERROR,
            errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
            errmsg("chdb_search: query is too large to send to the worker")
        );
    }
}
