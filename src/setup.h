#ifndef CHDB_SETUP_H
#define CHDB_SETUP_H

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Where the backend hands the helper its setup payload. */
#define CHDB_SETUP_FD 3

/* Type of query we ask the helper to execute. */
typedef uint8_t chdbCmdType;
#define CHDB_CMD_SELECT 'S'   /* SELECT, COPY FROM */
#define CHDB_CMD_INSERT 'I'   /* COPY TO */
#define CHDB_CMD_DESCRIBE 'D' /* DESCRIBE, CREATE TABLE */

/* Contextual information for the helper. */
typedef struct chdbHelperContext {
    chdbCmdType cmd;      /* Type of query to execute. */
    uint16_t max_memory;  /* max_memory_usage setting in MB, 0 for none */
    uint16_t max_threads; /* max_threads setting, 0 for none */
    uint16_t max_parsers; /* max_parsing_threads setting, 0 for none */
} chdbHelperContext;

/*
 * The SET every chDB session runs before its query, fixing the Native format
 * native.c decodes and applying the context's limits. The arguments are
 * max_threads, max_parsers and CHDB_SESSION_MEMORY_BYTES(max_memory).
 */
#define CHDB_SESSION_SETTINGS_FMT                                                      \
    "SET allow_experimental_nullable_tuple_type,"                                      \
    "output_format_json_quote_denormals,"                                              \
    "output_format_native_write_json_as_string,"                                       \
    "output_format_native_encode_types_in_binary_format=0,"                            \
    "date_time_output_format='iso',"                                                   \
    "max_threads=%" PRIu16 ",max_parsing_threads=%" PRIu16                             \
    ",max_memory_usage=%" PRIu64
#define CHDB_SESSION_MEMORY_BYTES(mb) ((uint64_t)(mb) * 1024 * 1024)

/*
 * The payload the backend writes to CHDB_SETUP_FD and then closes. The helper
 * reads it whole before touching either data channel.
 * Fields are native endian.
 *
 *   chdbHelperContext      command type, settings
 *   string                 query
 *   uint16                 parameter count
 *   string                 parameter name and value, repeated
 *
 * A string is a uint32 byte count followed by that many bytes, unterminated.
 */

/* Refuse a payload larger than this rather than sizing a buffer from it. */
#define CHDB_SETUP_MAX (16 * 1024 * 1024)

/*
 * Largest chunk of a chunked data stream either side will take, so a corrupt
 * count cannot size a buffer: the framing of channel.h, which search/protocol.h
 * carries between processes.
 */
#define CHDB_CHUNK_MAX (8 * 1024 * 1024)

/* Exit status telling the backend execution broke rather than the query. */
#define CHDB_HELPER_LOST_BACKEND 2

/*
 * Decoding the payload, for every program that reads one. The decoders are
 * bounds-checked, allocate nothing and borrow every string from the payload,
 * which the caller holds for as long as it uses them. Each returns false where
 * the payload ends early, leaving the cursor where it stopped.
 */
typedef struct chdbSetupCursor {
    const char* at;
    const char* end;
} chdbSetupCursor;

/* A borrowed, unterminated string. */
typedef struct chdbSetupStr {
    const char* data;
    size_t len;
} chdbSetupStr;

/* Copies the next `n` bytes into `out`. */
static inline bool
chdb_setup_take(chdbSetupCursor* c, void* out, size_t n) {
    if ((size_t)(c->end - c->at) < n) {
        return false;
    }
    memcpy(out, c->at, n);
    c->at += n;

    return true;
}

/* Takes the next string: a uint32 byte count and that many bytes. */
static inline bool
chdb_setup_take_str(chdbSetupCursor* c, chdbSetupStr* s) {
    uint32_t len;

    if (!chdb_setup_take(c, &len, sizeof(len)) || (size_t)(c->end - c->at) < len) {
        return false;
    }
    s->data = c->at;
    s->len  = len;
    c->at += len;

    return true;
}

/*
 * Takes everything ahead of the parameters: the context, the query and the
 * parameter count. The cursor is left at the first parameter.
 */
static inline bool
chdb_setup_parse_head(
    chdbSetupCursor* c,
    chdbHelperContext* ctx,
    chdbSetupStr* query,
    uint16_t* nparams
) {
    return chdb_setup_take(c, &ctx->cmd, sizeof(ctx->cmd)) &&
           chdb_setup_take(c, &ctx->max_memory, sizeof(ctx->max_memory)) &&
           chdb_setup_take(c, &ctx->max_threads, sizeof(ctx->max_threads)) &&
           chdb_setup_take(c, &ctx->max_parsers, sizeof(ctx->max_parsers)) &&
           chdb_setup_take_str(c, query) &&
           chdb_setup_take(c, nparams, sizeof(*nparams));
}

/* Takes the next parameter's name and value. */
static inline bool
chdb_setup_next_param(chdbSetupCursor* c, chdbSetupStr* name, chdbSetupStr* value) {
    return chdb_setup_take_str(c, name) && chdb_setup_take_str(c, value);
}

#endif /* CHDB_SETUP_H */
