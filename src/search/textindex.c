/*
 * The ClickHouse text skip index of a column: the `TYPE text(...)` arguments
 * rendered from the column's operator class options, and the table setting
 * phrase search needs. The options themselves are declared in options.c.
 */

#include "postgres.h"

#include <string.h>

#include "mb/pg_wchar.h"

#include "options.h"
#include "query.h"
#include "search.h"

/*
 * The arguments of `TYPE text(...)` for the column, from its operator class
 * options. Only the allowlisted names (or the superuser's raw expression)
 * reach the statement, and the column's name is already quoted.
 */
char*
chdb_search_skip_index_args(Relation index, int attno, const ChdbColumn* column) {
    bytea** all         = RelationGetIndexAttOptions(index, false);
    ChdbTextOptions* o  = all ? (ChdbTextOptions*)all[attno - 1] : NULL;
    const char* col     = column->name;
    ChdbColumnKind kind = column->kind;
    StringInfoData buf;

    /* The kind came from the proc that declared these, so they are ours. */
    Assert(!o || kind != CHDB_COL_TEXT || VARSIZE(o) >= sizeof(ChdbTextOptions));
    initStringInfo(&buf);
    if (kind == CHDB_COL_TEXT_ARRAY) {
        /* Elements compare in lower case, as chdb.has_token(text[], text) does. */
        appendStringInfo(
            &buf, "tokenizer = array, preprocessor = %s(%s)", DEFAULT_PREPROCESSOR, col
        );
        return buf.data;
    }

    const char* tok =
        (o && o->tokenizer) ? GET_STRING_RELOPTION(o, tokenizer) : DEFAULT_TOKENIZER;
    const char* pre = (o && o->preprocessor) ? GET_STRING_RELOPTION(o, preprocessor)
                                             : DEFAULT_PREPROCESSOR;

    if (strcmp(tok, "icu") == 0 || strcmp(tok, "splitByRegexp") == 0) {
        /* The argument is a literal, escaped like any other string. */
        appendStringInfo(&buf, "tokenizer = %s(", tok);
        chdb_search_append_string(&buf, GET_STRING_RELOPTION(o, tokenizer_arg));
        appendStringInfoChar(&buf, ')');
    } else if (strcmp(tok, "splitByString") == 0 && o && o->tokenizer_arg) {
        /* Each character is one separator. */
        appendStringInfoString(&buf, "tokenizer = splitByString([");
        for (const char* c = GET_STRING_RELOPTION(o, tokenizer_arg); *c;
             c += pg_mblen(c)) {
            if (c != GET_STRING_RELOPTION(o, tokenizer_arg)) {
                appendStringInfoString(&buf, ", ");
            }
            chdb_search_append_string(&buf, pnstrdup(c, pg_mblen(c)));
        }
        appendStringInfoString(&buf, "])");
    } else if (strcmp(tok, "ngrams") == 0) {
        appendStringInfo(
            &buf,
            "tokenizer = ngrams(%d)",
            o && o->ngram_size ? o->ngram_size : DEFAULT_NGRAM_SIZE
        );
    } else {
        appendStringInfo(&buf, "tokenizer = %s", tok);
    }

    if (o && o->raw_preprocessor) {
        appendStringInfo(
            &buf, ", preprocessor = %s", GET_STRING_RELOPTION(o, raw_preprocessor)
        );
    } else if (strcmp(pre, "none") != 0) {
        appendStringInfo(&buf, ", preprocessor = %s(%s)", pre, col);
    }
    if (o && o->support_phrase_search) {
        appendStringInfoString(&buf, ", support_phrase_search = 1");
    }
    return buf.data;
}

/* Whether any text column asks for phrase search, which needs a table setting. */
bool
chdb_search_wants_phrase_search(Relation index, const ChdbColumn* cols) {
    bytea** all = RelationGetIndexAttOptions(index, false);

    for (int i = 0; all && i < index->rd_att->natts; i++) {
        ChdbTextOptions* o = (ChdbTextOptions*)all[i];

        /* Only a text column's options have the field. */
        if (cols[i].kind == CHDB_COL_TEXT && o && o->support_phrase_search) {
            return true;
        }
    }
    return false;
}
