/*
 * SQL-callable pieces: the Postgres implementations of the token predicates
 * and the selectivity estimators.
 *
 * The predicates exist so that a sequential scan, or the heap recheck of a
 * plan that does not use the index, gives the answer the index would. They
 * implement ClickHouse's default pipeline only: lowerUTF8 (here Unicode's
 * own lowercase mapping, not the cluster's ctype) followed by
 * splitByNonAlpha, which breaks a string at every ASCII character that is
 * not a letter or digit and keeps bytes >= 0x80 inside words. An index built
 * with another tokenizer or preprocessor answers differently, so for those
 * the operators are meaningful through the index only. A needle without
 * tokens matches nothing, as in ClickHouse.
 */

#include "postgres.h"

#include <string.h>

#include "catalog/pg_type_d.h"
#include "common/unicode_case.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/lsyscache.h"

#include "query.h"
#include "search.h"

typedef struct Tok {
    const char* s;
    int n;
} Tok;

static bool
is_word_byte(unsigned char c) {
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

/* PostgreSQL 17 has the simple mapping only; 18 adds the full one. */
static size_t
strlower(char* dst, size_t size, const char* s, size_t n) {
#if PG_VERSION_NUM >= 180000
    return unicode_strlower(dst, size, s, n, true);
#else
    return unicode_strlower(dst, size, s, n);
#endif
}

/*
 * Lowercases as the index's lowerUTF8 does, by Unicode's full mapping (so
 * that İ becomes i̇, as ICU has it) whatever the cluster's ctype, under
 * which str_tolower is ASCII-only for C and POSIX. A database in another
 * encoding has no Unicode text to map and lowercases ASCII only.
 */
char*
chdb_search_lower(const char* s, size_t n) {
    if (GetDatabaseEncoding() != PG_UTF8) {
        return asc_tolower(s, n);
    }

    size_t len = strlower(NULL, 0, s, n);
    char* low  = palloc(len + 1);

    strlower(low, len + 1, s, n);
    return low;
}

/* Lowercases and splits `t`; token pointers live in the returned buffer. */
static int
tokenize(text* t, Tok** out) {
    char* low = chdb_search_lower(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
    int cap = 16, n = 0;
    Tok* toks = palloc(sizeof(Tok) * cap);

    for (char* p = low; *p;) {
        if (!is_word_byte((unsigned char)*p)) {
            p++;
            continue;
        }
        char* start = p;

        while (*p && is_word_byte((unsigned char)*p)) {
            p++;
        }
        if (n == cap) {
            cap *= 2;
            toks = repalloc(toks, sizeof(Tok) * cap);
        }
        toks[n++] = (Tok){ start, (int)(p - start) };
    }
    *out = toks;
    return n;
}

static bool
tok_eq(Tok a, Tok b) {
    return a.n == b.n && memcmp(a.s, b.s, a.n) == 0;
}

static bool
contains(Tok* hay, int nh, Tok needle) {
    for (int i = 0; i < nh; i++) {
        if (tok_eq(hay[i], needle)) {
            return true;
        }
    }
    return false;
}

static bool
all_tokens(text* hay, text* needles) {
    Tok *h, *n;
    int nh = tokenize(hay, &h), nn = tokenize(needles, &n);

    if (nn == 0) {
        return false; /* as ClickHouse: no needle tokens match nothing */
    }
    for (int i = 0; i < nn; i++) {
        if (!contains(h, nh, n[i])) {
            return false;
        }
    }
    return true;
}

static bool
any_tokens(text* hay, text* needles) {
    Tok *h, *n;
    int nh = tokenize(hay, &h), nn = tokenize(needles, &n);

    for (int i = 0; i < nn; i++) {
        if (contains(h, nh, n[i])) {
            return true;
        }
    }
    return false;
}

static bool
phrase(text* hay, text* needles) {
    Tok *h, *n;
    int nh = tokenize(hay, &h), nn = tokenize(needles, &n);

    if (nn == 0) {
        return false;
    }
    for (int i = 0; i + nn <= nh; i++) {
        int j = 0;

        while (j < nn && tok_eq(h[i + j], n[j])) {
            j++;
        }
        if (j == nn) {
            return true;
        }
    }
    return false;
}

/*
 * ClickHouse's hasToken rejects a needle that is not one token, judging the
 * needle as written: any separator byte in it raises, and an empty needle
 * matches nothing. (Lowercasing first would misjudge it: İstanbul shrinks
 * by a byte, and the length test took that for a second token.)
 */
static bool
single_token(text* hay, text* needle) {
    const char* raw = VARDATA_ANY(needle);
    size_t len      = VARSIZE_ANY_EXHDR(needle);
    Tok *h, *n;
    int nh, nn PG_USED_FOR_ASSERTS_ONLY;

    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!is_word_byte((unsigned char)raw[i])) {
            ereport(
                ERROR,
                errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("has_token needs a needle that is a single token")
            );
        }
    }
    nh = tokenize(hay, &h);
    nn = tokenize(needle, &n);
    Assert(nn == 1);
    return contains(h, nh, n[0]);
}

#define TEXT_PREDICATE(name, impl)                                                     \
    PG_FUNCTION_INFO_V1(name);                                                         \
    Datum name(PG_FUNCTION_ARGS) {                                                     \
        PG_RETURN_BOOL(impl(PG_GETARG_TEXT_PP(0), PG_GETARG_TEXT_PP(1)));              \
    }

TEXT_PREDICATE(chdb_search_has_all_tokens, all_tokens)
TEXT_PREDICATE(chdb_search_has_any_tokens, any_tokens)
TEXT_PREDICATE(chdb_search_has_token, single_token)
TEXT_PREDICATE(chdb_search_has_phrase, phrase)

/*
 * The array tokenizer makes each element one token, compared after the
 * preprocessor. The needle is one token too, so all, any and token coincide.
 */
PG_FUNCTION_INFO_V1(chdb_search_array_has_token);
Datum
chdb_search_array_has_token(PG_FUNCTION_ARGS) {
    ArrayType* arr   = PG_GETARG_ARRAYTYPE_P(0);
    text* t          = PG_GETARG_TEXT_PP(1);
    char* needle     = chdb_search_lower(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
    ArrayIterator it = array_create_iterator(arr, 0, NULL);
    Datum d;
    bool isnull, found = false;

    while (!found && array_iterate(it, &d, &isnull)) {
        if (!isnull) {
            text* e = DatumGetTextPP(d);

            found = strcmp(
                        chdb_search_lower(VARDATA_ANY(e), VARSIZE_ANY_EXHDR(e)), needle
                    ) == 0;
        }
    }
    array_free_iterator(it);
    PG_RETURN_BOOL(found);
}

/* Operator estimators: the index decides, the planner only needs "selective". */
PG_FUNCTION_INFO_V1(chdb_search_restrict_sel);
Datum
chdb_search_restrict_sel(PG_FUNCTION_ARGS) {
    PG_RETURN_FLOAT8(0.01);
}

PG_FUNCTION_INFO_V1(chdb_search_join_sel);
Datum
chdb_search_join_sel(PG_FUNCTION_ARGS) {
    PG_RETURN_FLOAT8(0.01);
}
