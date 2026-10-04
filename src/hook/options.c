/*
 * The chDB options of a COPY or CREATE TABLE, read into a chdbCopyContext:
 * the credentials, format, structure and compression the table function
 * takes, the timeout, and how encoding errors are handled. The chdb_hook
 * document lists them.
 */

#include "postgres.h"

#include "commands/defrem.h"

#include "options.h"

/*
 * Copy of `structure` with every newline turned into a space
 * Workaround for https://github.com/chdb-io/chdb-core/issues/158
 */
static char*
structure_on_one_line(const char* structure) {
    char* flat = pstrdup(structure);

    for (char* cursor = flat; *cursor != '\0'; cursor++) {
        if (*cursor == '\n' || *cursor == '\r') {
            *cursor = ' ';
        }
    }

    return flat;
}

/*
 * Fill `ctx` from `options`
 * Collect options unused by chDB in `*others`, or reject them when `others` is NULL
 */
void
chdb_copy_options(chdbCopyContext* ctx, List* options, List** others) {
    ListCell* lc;
    ctx->access_key     = "";
    ctx->access_secret  = "";
    ctx->session_token  = "";
    ctx->format         = "";
    ctx->structure      = "";
    ctx->compression    = "";
    ctx->timeout        = 30000; /* Same as ClickHouse. */
    ctx->encoding_check = CHC_ENC_FAIL;

    foreach (lc, options) {
        DefElem* elem = (DefElem*)lfirst(lc);
        if (strcmp(elem->defname, "access_key") == 0) {
            ctx->access_key = defGetString(elem);
        } else if (strcmp(elem->defname, "access_secret") == 0) {
            ctx->access_secret = defGetString(elem);
        } else if (strcmp(elem->defname, "session_token") == 0) {
            ctx->session_token = defGetString(elem);
        } else if (strcmp(elem->defname, "format") == 0) {
            ctx->format = defGetString(elem);
        } else if (strcmp(elem->defname, "structure") == 0) {
            ctx->structure = structure_on_one_line(defGetString(elem));
        } else if (strcmp(elem->defname, "compression") == 0) {
            ctx->compression = defGetString(elem);
        } else if (strcmp(elem->defname, "timeout") == 0) {
            int64 timeout = defGetInt64(elem);
            if (timeout < 0 || timeout > UINT32_MAX) {
                ereport(
                    ERROR,
                    errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("chdb: argument to COPY option \"timeout\" must be a uint32")
                );
            }
            ctx->timeout = (uint32_t)timeout;
        } else if (strcmp(elem->defname, "encoding_check") == 0) {
            const char* val = defGetString(elem);
            pgch_encoding_check v;

            if (!pgch_parse_encoding_check(val, &v)) {
                ereport(
                    ERROR,
                    errcode(ERRCODE_FDW_INVALID_STRING_FORMAT),
                    errmsg("invalid value for option \"encoding_check\": \"%s\"", val),
                    errhint("Valid values are: fail, truncate, remove, replace")
                );
            }
            ctx->encoding_check = v;
        } else if (others) {
            *others = lappend(*others, elem);
        } else {
            ereport(
                ERROR,
                errcode(ERRCODE_SYNTAX_ERROR),
                errmsg("chdb: option \"%s\" not supported", elem->defname)
            );
        }
    }
}
