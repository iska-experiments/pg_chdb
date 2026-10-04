/*
 * Framing a request of protocol.h: the byte count, the index OID and store
 * generation, then the setup payload of src/setup.h as chdb_helper_build_setup
 * writes it for chdb_helper; and reading the status frame that answers it.
 * client.c sends the frame to the worker over its socket; sweep.c sends it
 * from the worker to its engine. See frame.h.
 */

#include "postgres.h"

#include "../helper.h"
#include "frame.h"
#include "worker.h"

void
chdb_search_frame_request(
    StringInfo buf,
    chdbCmdType cmd,
    Oid index,
    uint64 generation,
    const char* sql
) {
    uint32_t len          = 0;
    int start             = buf->len;
    chdbHelperContext ctx = {
        .cmd         = cmd,
        .max_memory  = (uint16_t)chdb_max_memory,
        .max_threads = (uint16_t)chdb_max_threads,
        .max_parsers = (uint16_t)chdb_max_parsers,
    };

    appendBinaryStringInfo(buf, (char*)&len, sizeof(len)); /* patched below */
    appendBinaryStringInfo(buf, (char*)&index, sizeof(index));
    appendBinaryStringInfo(buf, (char*)&generation, sizeof(generation));
    chdb_helper_build_setup(buf, &ctx, sql, NULL, NULL, 0);
    len = (uint32_t)(buf->len - start - sizeof(len));
    memcpy(buf->data + start, &len, sizeof(len));
}

char*
chdb_search_frame_status(
    chdbChannel* ch,
    const char* peer,
    const char* query,
    uint8_t* status
) {
    uint32_t len;

    chdb_channel_recv_exact(ch, status, sizeof(*status));
    chdb_channel_recv_exact(ch, &len, sizeof(len));
    if (len > CHDB_CHUNK_MAX) {
        /* The framing cannot be followed past this, so the connection goes. */
        chdb_channel_close(ch);
        ereport(
            ERROR,
            errcode(ERRCODE_PROTOCOL_VIOLATION),
            errmsg("chdb_search: the %s sent a malformed status", peer),
            errdetail("Its text would be %u bytes long.", len),
            errcontext("query: %s", query)
        );
    }

    char* text = palloc(len + 1);

    chdb_channel_recv_exact(ch, text, len);
    text[len] = '\0';

    return text;
}

void
chdb_search_frame_skip_data(chdbChannel* ch) {
    char skip[8192];

    while (chdb_channel_recv(ch, skip, sizeof(skip))) {}
}
