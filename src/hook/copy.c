/*
 * Running a COPY through a chDB helper process: the structure clause of the
 * columns it moves, the query of tablefunc.c, and the Native stream of
 * native.h in the direction the command wants.
 */

#include "postgres.h"

#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/rel.h"

/* PGCH_NATIVE_SETTINGS; native.c is the TU carrying the implementation. */
#include "pg-clickhouse.h"

#include "../helper.h"
#include "../native.h"
#include "../native_writer.h"
#include "copy.h"
#include "tablefunc.h"

/*
 * Structure clause for the columns `attnums` names, similar to how
 * pgch_structure_from_tupdesc builds for a whole relation. A COPY column list
 * decides which columns cross, so the clause has to follow it.
 */
static char*
structure_for_attnums(TupleDesc desc, List* attnums) {
    StringInfoData buf;

    initStringInfo(&buf);
    ListCell* lc;
    foreach (lc, attnums) {
        Form_pg_attribute attr = TupleDescAttr(desc, lfirst_int(lc) - 1);

        if (buf.len) {
            appendStringInfoString(&buf, ", ");
        }
        appendStringInfo(
            &buf,
            "%s %s",
            pgch_quote_ch_ident(NameStr(attr->attname)),
            chdb_ch_type_for(attr->atttypid, attr->atttypmod, attr->attnotnull)
        );
    }

    return buf.data;
}

/*
 * Return a copy of `structure` with every bare `type` clause replaced with
 * `String`.
 */
static char*
structure_as_string(const char* structure, const char* type) {
    StringInfoData buf;
    size_t len  = strlen(type);
    bool quoted = false;

    initStringInfo(&buf);
    for (const char* pos = structure; *pos;) {
        if (*pos == '"') {
            quoted = !quoted;
        }
        if (!quoted && strncmp(pos, type, len) == 0 && pos > structure &&
            (pos[-1] == ' ' || pos[-1] == '(') &&
            (pos[len] == '\0' || pos[len] == ',' || pos[len] == ')')) {
            appendStringInfoString(&buf, "String");
            pos += len;
        } else {
            appendStringInfoChar(&buf, *pos++);
        }
    }
    return buf.data;
}

/*
 * Returns true if `format` is one of the formats lacking Time64 support.
 * In such cases, `Time64(6)` should be replaced with `String`.
 */
static bool
format_lacks_time64(const char* format) {
    static const char* const formats[] = {
        "Parquet",  "Arrow",        "ArrowStream", "ORC",         "Avro",
        "Protobuf", "ProtobufList", "MsgPack",     "BSONEachRow",
    };

    for (size_t i = 0; i < lengthof(formats); i++) {
        if (pg_strcasecmp(format, formats[i]) == 0) {
            return true;
        }
    }
    return false;
}

uint64_t
chdb_copy(chdbCopyContext* ctx) {
    /*
     * A value with no ClickHouse representation falls back to its Postgres
     * output function, which reads both of these GUCs. Take the settings for the
     * copy alone: transaction end restores what the session had.
     */
    int nestlevel = NewGUCNestLevel();

    /* We always need a structure. */
    if (ctx->structure[0] == '\0') {
        TupleDesc desc = RelationGetDescr(ctx->rel);

        ctx->structure = structure_for_attnums(desc, ctx->attnums);
        if (pg_strcasecmp(ctx->format, "ORC") == 0) {
            ctx->structure = structure_as_string(ctx->structure, "UUID");
        }
        if (format_lacks_time64(ctx->format)) {
            ctx->structure = structure_as_string(ctx->structure, "Time64(6)");
        }
    }

    /* Assemble the chDB query. */
    StringInfoData ch_query;
    initStringInfo(&ch_query);

    char* names[CHDB_MAX_TABLEFUNC_ARGS];
    char* values[CHDB_MAX_TABLEFUNC_ARGS];
    size_t param_count = chdb_table_function_query(ctx, &ch_query, names, values);

    /* Hand off to the helper. */
    chdbHelperContext hcx = {
        .cmd         = ctx->cmd_type,
        .max_memory  = ctx->max_memory,
        .max_threads = ctx->max_threads,
        .max_parsers = ctx->max_parsers,
    };
    chdbChannel* helper =
        chdb_helper_start(&hcx, ch_query.data, names, values, param_count);
    uint64_t num_rows =
        ctx->cmd_type == CHDB_CMD_SELECT
            ? chdb_copy_receive(
                  ctx->rel,
                  ctx->attnums,
                  ctx->rtable,
                  ctx->rteperminfos,
                  ctx->encoding_check,
                  helper
              )
            : chdb_copy_send(ctx->rel, ctx->structure, ctx->attnums, helper);
    chdb_helper_finish(helper);

    AtEOXact_GUC(true, nestlevel);

    return num_rows;
}

List*
chdb_describe(chdbCopyContext* ctx) {
    /* Build DESCRIBE with read settings without changing COPY context */
    chdbCopyContext describe = *ctx;
    StringInfoData ch_query;
    char* names[CHDB_MAX_TABLEFUNC_ARGS];
    char* values[CHDB_MAX_TABLEFUNC_ARGS];

    describe.cmd_type = CHDB_CMD_DESCRIBE;
    /* Ask chDB to infer structure when none was provided */
    if (describe.structure[0] == '\0') {
        describe.structure = "auto";
    }
    initStringInfo(&ch_query);

    size_t param_count = chdb_table_function_query(&describe, &ch_query, names, values);
    chdbHelperContext hcx = {
        .cmd         = describe.cmd_type,
        .max_memory  = describe.max_memory,
        .max_threads = describe.max_threads,
        .max_parsers = describe.max_parsers,
    };
    chdbChannel* helper =
        chdb_helper_start(&hcx, ch_query.data, names, values, param_count);
    List* columns = chdb_native_describe(helper);

    chdb_helper_finish(helper);

    return columns;
}
