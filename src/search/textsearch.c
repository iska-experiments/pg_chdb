/*
 * One text search of one column as a ClickHouse expression: the token
 * functions of the text index for the token strategies, and for a regular
 * expression or a LIKE pattern, which read the text rather than the posting
 * lists, the column through its preprocessor with the pattern folded alike.
 */

#include "postgres.h"

#include "query.h"
#include "search.h"

/*
 * ClickHouse's hasToken takes no array. The array tokenizer makes the needle
 * one token, so for arrays all, any and token agree and hasAllTokens serves;
 * a phrase or a pattern over elements has no meaning, and ClickHouse would
 * reject it.
 */
static const char*
token_function(StrategyNumber strategy, ChdbColumnKind kind) {
    if (kind == CHDB_COL_TEXT_ARRAY && strategy == CHDB_STRATEGY_HAS_TOKEN) {
        return "hasAllTokens";
    }
    switch (strategy) {
    case CHDB_STRATEGY_HAS_ALL_TOKENS:
        return "hasAllTokens";
    case CHDB_STRATEGY_HAS_ANY_TOKENS:
        return "hasAnyTokens";
    case CHDB_STRATEGY_HAS_TOKEN:
        return "hasToken";
    case CHDB_STRATEGY_HAS_PHRASE:
        return "hasPhrase";
    }
    elog(ERROR, "unknown chdb text strategy %d", strategy);
}

/*
 * A text search of `col` by the strategy of its operator. The token
 * searches are the index's own functions, which preprocess the needle as
 * they do the column; a phrase with slop is phrase.c's. A regular expression and a LIKE
 * pattern read the text, so they take the column through its preprocessor, with the
 * pattern folded alike: match() by RE2's (?i), LIKE through the same function. A LIKE
 * is parenthesized, as it may follow a NOT.
 */
void
chdb_search_append_text_search(
    StringInfo buf,
    const ChdbColumn* col,
    StrategyNumber strategy,
    const char* needle,
    int32 slop
) {
    StringInfoData lit;

    if (col->kind == CHDB_COL_TEXT_ARRAY) {
        chdb_search_check_array_search(strategy);
    }
    if (strategy == CHDB_STRATEGY_HAS_PHRASE && slop) {
        chdb_search_append_slop_phrase(buf, col, needle, slop);
        return;
    }
    initStringInfo(&lit);
    if (strategy == CHDB_STRATEGY_REGEX && chdb_search_folds_case(col)) {
        needle = psprintf("(?i)%s", needle);
    }
    chdb_search_append_string(&lit, needle);
    switch (strategy) {
    case CHDB_STRATEGY_REGEX:
        appendStringInfo(
            buf,
            "match(%s, %s)",
            chdb_search_preprocessed(col, col->name, false),
            lit.data
        );
        return;
    case CHDB_STRATEGY_WILDCARD:
        appendStringInfo(
            buf,
            "(%s LIKE %s)",
            chdb_search_preprocessed(col, col->name, false),
            chdb_search_folds_case(col) ? chdb_search_preprocessed(col, lit.data, true)
                                        : lit.data
        );
        return;
    }
    appendStringInfo(
        buf, "%s(%s, %s)", token_function(strategy, col->kind), col->name, lit.data
    );
}
