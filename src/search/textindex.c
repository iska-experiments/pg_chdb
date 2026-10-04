/*
 * The ClickHouse text skip index of a column: the `TYPE text(...)` arguments
 * rendered from the column's operator class options, the table setting
 * phrase search needs, and the column's tokenizer and preprocessor as
 * expressions. The options themselves are declared in options.c.
 *
 * The tokenizer and the preprocessor are also what a query needs to see a
 * string as the index sees it: tokens() takes the tokenizer as the DDL
 * spells it, `ngrams(3)` or `splitByString([' '])`, and a search that reads
 * the text rather than the posting lists, or a text search in a SELECT
 * list, which ClickHouse evaluates without the index, takes the column
 * through the preprocessor by hand.
 */

#include "postgres.h"

#include <string.h>

#include "mb/pg_wchar.h"

#include "options.h"
#include "query.h"
#include "search.h"

/* The text options of a column, which only a text_ops column has. */
static const ChdbTextOptions*
text_options(const ChdbColumn* column) {
    /* The kind came from the proc that declared these, so they are ours. */
    Assert(
        !column->options || column->kind != CHDB_COL_TEXT ||
        VARSIZE(column->options) >= sizeof(ChdbTextOptions)
    );
    return column->kind == CHDB_COL_TEXT ? (ChdbTextOptions*)column->options : NULL;
}

/*
 * The allowlisted preprocessor, "none" included. A text[] column's elements
 * compare in lower case, as chdb.has_token(text[], text) does.
 */
static const char*
preprocessor_of(const ChdbTextOptions* o) {
    return (o && o->preprocessor) ? GET_STRING_RELOPTION(o, preprocessor)
                                  : DEFAULT_PREPROCESSOR;
}

static const char*
tokenizer_of(const ChdbTextOptions* o) {
    return (o && o->tokenizer) ? GET_STRING_RELOPTION(o, tokenizer) : DEFAULT_TOKENIZER;
}

/*
 * The tokenizer's arguments as ClickHouse writes them, in the index DDL as
 * `tokenizer = name(args)` and in a tokens() call as `tokens(s, 'name',
 * args)`: a literal for icu and splitByRegexp, escaped like any other
 * string; each character of splitByString's as one separator; the gram
 * length for ngrams. NULL when the tokenizer takes none.
 */
static const char*
tokenizer_args(const ChdbTextOptions* o, const char* tok) {
    StringInfoData buf;

    initStringInfo(&buf);
    if (strcmp(tok, "icu") == 0 || strcmp(tok, "splitByRegexp") == 0) {
        chdb_search_append_string(&buf, GET_STRING_RELOPTION(o, tokenizer_arg));
    } else if (strcmp(tok, "splitByString") == 0 && o && o->tokenizer_arg) {
        const char* seps = GET_STRING_RELOPTION(o, tokenizer_arg);

        appendStringInfoChar(&buf, '[');
        for (const char* c = seps; *c; c += pg_mblen(c)) {
            if (c != seps) {
                appendStringInfoString(&buf, ", ");
            }
            chdb_search_append_string(&buf, pnstrdup(c, pg_mblen(c)));
        }
        appendStringInfoChar(&buf, ']');
    } else if (strcmp(tok, "ngrams") == 0) {
        appendStringInfo(
            &buf, "%d", o && o->ngram_size ? o->ngram_size : DEFAULT_NGRAM_SIZE
        );
    } else {
        return NULL;
    }
    return buf.data;
}

/* The column's tokenizer and its arguments, as tokenizer_args gives them. */
static const char*
tokenizer_parts(const ChdbColumn* column, const char** args) {
    const ChdbTextOptions* o = text_options(column);
    const char* tok = column->kind == CHDB_COL_TEXT_ARRAY ? "array" : tokenizer_of(o);

    *args = column->kind == CHDB_COL_TEXT_ARRAY ? NULL : tokenizer_args(o, tok);
    return tok;
}

/*
 * The tokenizer as `TYPE text(tokenizer = ...)` spells it, and as a text
 * search names it in its third argument. Only the allowlisted names reach
 * the statement.
 */
char*
chdb_search_tokenizer(const ChdbColumn* column) {
    const char* args;
    const char* tok = tokenizer_parts(column, &args);

    return args ? psprintf("%s(%s)", tok, args) : pstrdup(tok);
}

/*
 * The arguments of `TYPE text(...)` for the column, from its operator class
 * options. Only the allowlisted names (or the superuser's raw expression)
 * reach the statement, and the column's name is already quoted.
 */
char*
chdb_search_skip_index_args(const ChdbColumn* column) {
    const ChdbTextOptions* o = text_options(column);
    const char* pre          = preprocessor_of(o);
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfo(&buf, "tokenizer = %s", chdb_search_tokenizer(column));
    if (o && o->raw_preprocessor) {
        appendStringInfo(
            &buf, ", preprocessor = %s", GET_STRING_RELOPTION(o, raw_preprocessor)
        );
    } else if (strcmp(pre, "none") != 0) {
        appendStringInfo(&buf, ", preprocessor = %s(%s)", pre, column->name);
    }
    if (o && o->support_phrase_search) {
        appendStringInfoString(&buf, ", support_phrase_search = 1");
    }
    return buf.data;
}

/* Whether any text column asks for phrase search, which needs a table setting. */
bool
chdb_search_wants_phrase_search(const ChdbColumn* cols, int natts) {
    for (int i = 0; i < natts; i++) {
        const ChdbTextOptions* o = text_options(&cols[i]);

        if (o && o->support_phrase_search) {
            return true;
        }
    }
    return false;
}

/*
 * Where ClickHouse does not apply the index's preprocessor itself: over a
 * text[] column, element by element, which the DDL form leaves to the
 * index, and over its needle as one string.
 */
char*
chdb_search_preprocessed(const ChdbColumn* column, const char* expr, bool literal) {
    const ChdbTextOptions* o = text_options(column);
    const char* pre          = preprocessor_of(o);

    if (column->kind == CHDB_COL_TEXT_ARRAY && !literal) {
        return psprintf("arrayMap(x -> %s(x), %s)", pre, expr);
    }
    if (o && o->raw_preprocessor) {
        const char* raw = GET_STRING_RELOPTION(o, raw_preprocessor);

        /*
         * The expression names the column, so a literal takes the column's
         * name in a subquery, as ClickHouse itself preprocesses a needle.
         */
        return literal
                   ? psprintf(
                         "(SELECT %s FROM (SELECT %s AS %s))", raw, expr, column->name
                     )
                   : pstrdup(raw);
    }
    return strcmp(pre, "none") == 0 ? pstrdup(expr) : psprintf("%s(%s)", pre, expr);
}

bool
chdb_search_folds_case(const ChdbColumn* column) {
    const ChdbTextOptions* o = text_options(column);

    if (column->kind == CHDB_COL_TEXT_ARRAY) {
        return true;
    }
    if (o && o->raw_preprocessor) {
        const char* raw = GET_STRING_RELOPTION(o, raw_preprocessor);

        /* Unknown; the expression is the superuser's, and most lowercase. */
        return strstr(raw, "lower") != NULL || strstr(raw, "caseFold") != NULL;
    }

    const char* pre = preprocessor_of(o);

    return strcmp(pre, "lower") == 0 || strcmp(pre, "lowerUTF8") == 0 ||
           strcmp(pre, "caseFoldUTF8") == 0;
}

/* The one tokens() call, for the score's needles and a phrase's positions. */
char*
chdb_search_tokens_call(const ChdbColumn* column, const char* expr, bool literal) {
    const char* args;
    const char* tok = tokenizer_parts(column, &args);

    return psprintf(
        "tokens(%s, '%s'%s%s)",
        chdb_search_preprocessed(column, expr, literal),
        tok,
        args ? ", " : "",
        args ? args : ""
    );
}
