/*
 * The Postgres implementations of the pattern predicates, chdb.regex and
 * chdb.wildcard, which match the text itself rather than its tokens. Like
 * the token predicates of ops.c they follow the default preprocessor,
 * lowerUTF8: the text is lowercased by Unicode and the pattern matched
 * without regard to case, as the index does for a column whose
 * preprocessor folds case (query.c renders match(lowerUTF8(col), '(?i)re')
 * and lowerUTF8(col) LIKE lowerUTF8('pattern')).
 *
 * The engines differ at the edges. A regular expression is RE2's in
 * ClickHouse and an ARE here, which agree on the everyday syntax and part
 * on lookahead (ARE only), \b (RE2 only) and the exact classes; case folds
 * by Unicode in RE2 and by the collation's ctype here, which under C folds
 * ASCII alone, while the lowercased text already covers a lowercase pattern.
 * In a LIKE pattern a backslash escapes %, _ and itself in both; before any
 * other character ClickHouse keeps the backslash and Postgres drops it.
 */

#include "postgres.h"

#include "catalog/pg_collation_d.h"
#include "fmgr.h"
#include "regex/regex.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"

#include "ops.h"
#include "search.h"

static text*
lowered(text* t) {
    return cstring_to_text(chdb_search_lower(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t)));
}

bool
chdb_search_regex_matches(text* hay, text* re, Oid collation) {
    char* low = chdb_search_lower(VARDATA_ANY(hay), VARSIZE_ANY_EXHDR(hay));

    return RE_compile_and_execute(
        re, low, strlen(low), REG_ADVANCED | REG_ICASE, collation, 0, NULL
    );
}

/* LIKE on the lowercased text and pattern; bytewise, as ClickHouse compares. */
bool
chdb_search_wildcard_matches(text* hay, text* pattern) {
    return DatumGetBool(DirectFunctionCall2Coll(
        textlike,
        C_COLLATION_OID,
        PointerGetDatum(lowered(hay)),
        PointerGetDatum(lowered(pattern))
    ));
}

PG_FUNCTION_INFO_V1(chdb_search_regex);
Datum
chdb_search_regex(PG_FUNCTION_ARGS) {
    PG_RETURN_BOOL(chdb_search_regex_matches(
        PG_GETARG_TEXT_PP(0), PG_GETARG_TEXT_PP(1), PG_GET_COLLATION()
    ));
}

PG_FUNCTION_INFO_V1(chdb_search_wildcard);
Datum
chdb_search_wildcard(PG_FUNCTION_ARGS) {
    PG_RETURN_BOOL(
        chdb_search_wildcard_matches(PG_GETARG_TEXT_PP(0), PG_GETARG_TEXT_PP(1))
    );
}
