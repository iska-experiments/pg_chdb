/*
 * SQL-callable pieces: the Postgres implementations of the search predicates,
 * chdb.tokens and the selectivity estimators.
 *
 * The predicates exist so that a sequential scan, or the heap recheck of a
 * plan that does not use the index, gives the answer the index would. They
 * implement ClickHouse's default pipeline only: lowerUTF8 (here the database's
 * lower()) followed by splitByNonAlpha, which breaks a string at every ASCII
 * character that is not a letter or digit and keeps bytes >= 0x80 inside
 * words. An index built with another tokenizer or preprocessor answers
 * differently, so for those the operators are meaningful through the index
 * only. A needle without tokens matches nothing, as in ClickHouse.
 */

#include "postgres.h"

#include <string.h>

#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/formatting.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "../native.h"
#include "pg-clickhouse-decode.h"
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

/* Lowercases and splits `t`; token pointers live in the returned buffer. */
static int
tokenize(text* t, Tok** out) {
    char* raw = text_to_cstring(t);
    char* low = str_tolower(raw, strlen(raw), DEFAULT_COLLATION_OID);
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

/* ClickHouse's hasToken rejects a needle that is not one token; so does this. */
static bool
single_token(text* hay, text* needle) {
    Tok *h, *n;
    int nh = tokenize(hay, &h), nn = tokenize(needle, &n);

    if (nn != 1 || n[0].n != (int)strlen(text_to_cstring(needle))) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("has_token needs a needle that is a single token")
        );
    }
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
    ArrayType* arr = PG_GETARG_ARRAYTYPE_P(0);
    char* needle   = str_tolower(
        text_to_cstring(PG_GETARG_TEXT_PP(1)),
        VARSIZE_ANY_EXHDR(PG_GETARG_TEXT_PP(1)),
        DEFAULT_COLLATION_OID
    );
    ArrayIterator it = array_create_iterator(arr, 0, NULL);
    Datum d;
    bool isnull, found = false;

    while (!found && array_iterate(it, &d, &isnull)) {
        if (!isnull) {
            char* e = text_to_cstring(DatumGetTextPP(d));

            found =
                strcmp(str_tolower(e, strlen(e), DEFAULT_COLLATION_OID), needle) == 0;
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

/* chdb.tokens(text): what the worker's tokens() makes of a string. */
PG_FUNCTION_INFO_V1(chdb_search_tokens);
Datum
chdb_search_tokens(PG_FUNCTION_ARGS) {
    StringInfoData sql;
    MemoryContext cxt = AllocSetContextCreate(
        CurrentMemoryContext, "chdb_search tokens", ALLOCSET_SMALL_SIZES
    );
    Datum result = (Datum)0;
    bool found   = false;

    initStringInfo(&sql);
    appendStringInfoString(&sql, "SELECT tokens(");
    chdb_search_append_string(&sql, text_to_cstring(PG_GETARG_TEXT_PP(0)));
    appendStringInfoChar(&sql, ')');

    /* tokens() is not tied to any index, so the request names none. */
    chdbSearchConn* conn = chdb_search_connect();
    MemoryContext old    = MemoryContextSwitchTo(cxt);

    PG_TRY();
    {
        pgch_reader reader;
        pgch_block_source src;

        chdb_search_log_sql("select", sql.data);
        chdb_search_select(conn, InvalidOid, sql.data);
        src = chdb_native_source(chdb_search_channel(conn));
        pgch_reader_init(&reader, &src);
        if (reader.error) {
            ereport(
                ERROR,
                errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                errmsg("chdb_search: %s", reader.error)
            );
        }
        if (pgch_reader_columns(&reader) == 1) {
            void* state = pgch_reader_convert_init(&reader, 0, TEXTARRAYOID, -1);
            Datum v;
            bool n;

            if (pgch_reader_next(&reader)) {
                pgch_reader_fill(&reader, &state, &v, &n);
                MemoryContextSwitchTo(old);
                result = n ? (Datum)0 : datumCopy(v, false, -1);
                found  = !n;
                MemoryContextSwitchTo(cxt);
            }
            if (reader.error) {
                ereport(
                    ERROR,
                    errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                    errmsg("chdb_search: %s", reader.error)
                );
            }
        }
        chdb_search_finish(conn);
    }
    PG_FINALLY();
    {
        MemoryContextSwitchTo(old);
        chdb_search_close(conn);
    }
    PG_END_TRY();
    MemoryContextDelete(cxt);

    if (!found) {
        PG_RETURN_ARRAYTYPE_P(construct_empty_array(TEXTOID));
    }
    PG_RETURN_DATUM(result);
}
