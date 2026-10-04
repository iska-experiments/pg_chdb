/*
 * Index and operator class options.
 *
 * Index-level reloptions are few: vacuum_optimize_ratio overrides the GUC.
 * Per-column options belong to the text_ops operator class, through the PG13+
 * options support function, so each column carries its own tokenizer. Names
 * that reach ClickHouse DDL are checked against allowlists here, because
 * they are spliced into the statement; raw_preprocessor is the escape hatch
 * and needs a superuser. textindex.c renders the options into the DDL.
 */

#include "postgres.h"

#include <string.h>

#include "fmgr.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include "options.h"
#include "search.h"

typedef struct ChdbIndexOptions {
    int32 vl_len_;
    double vacuum_optimize_ratio; /* negative: use the GUC */
} ChdbIndexOptions;

static relopt_kind chdb_relopt_kind;

static const char* const tokenizers[] = {
    "splitByNonAlpha", "splitByString", "splitByRegexp", "ngrams",
    "sparseGrams",     "icu",           "asciiCJK",      "array",
};

static const char* const preprocessors[] = {
    "lower", "lowerUTF8", "caseFoldUTF8", "extractTextFromHTML", "none",
};

void
chdb_search_init_options(void) {
    chdb_relopt_kind = add_reloption_kind();
    add_real_reloption(
        chdb_relopt_kind,
        "vacuum_optimize_ratio",
        "Dead fraction above which VACUUM runs OPTIMIZE ... FINAL; negative uses the "
        "GUC",
        -1.0,
        -1.0,
        1.0,
        AccessExclusiveLock
    );
}

bytea*
chdb_search_amoptions(Datum reloptions, bool validate) {
    static const relopt_parse_elt tab[] = {
        { "vacuum_optimize_ratio",
         RELOPT_TYPE_REAL, offsetof(ChdbIndexOptions, vacuum_optimize_ratio) },
    };

    return (bytea*)build_reloptions(
        reloptions,
        validate,
        chdb_relopt_kind,
        sizeof(ChdbIndexOptions),
        tab,
        lengthof(tab)
    );
}

double
chdb_search_index_optimize_ratio(Relation index) {
    ChdbIndexOptions* opts = (ChdbIndexOptions*)index->rd_options;

    if (opts && opts->vacuum_optimize_ratio >= 0) {
        return opts->vacuum_optimize_ratio;
    }
    return chdb_search_vacuum_optimize_ratio;
}

static void
check_allowed(const char* what, const char* value, const char* const* allowed, int n) {
    for (int i = 0; i < n; i++) {
        if (strcmp(value, allowed[i]) == 0) {
            return;
        }
    }
    StringInfoData list;

    initStringInfo(&list);
    for (int i = 0; i < n; i++) {
        appendStringInfo(&list, "%s%s", i ? ", " : "", allowed[i]);
    }
    ereport(
        ERROR,
        errcode(ERRCODE_INVALID_PARAMETER_VALUE),
        errmsg("invalid %s \"%s\" for a chdb index", what, value),
        errhint("Allowed values: %s.", list.data)
    );
}

static void
validate_tokenizer(const char* value) {
    check_allowed("tokenizer", value, tokenizers, lengthof(tokenizers));
}

static void
validate_preprocessor(const char* value) {
    check_allowed("preprocessor", value, preprocessors, lengthof(preprocessors));
}

/* The expression goes into ClickHouse DDL unchecked, so only trusted roles. */
static void
validate_raw_preprocessor(const char* value) {
    if (!value) {
        return;
    }
    if (!superuser()) {
        ereport(
            ERROR,
            errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
            errmsg("only superusers can set raw_preprocessor")
        );
    }
}

static void
validate_text_options(void* parsed, relopt_value* vals, int nvals) {
    ChdbTextOptions* o = parsed;
    const char* tok    = GET_STRING_RELOPTION(o, tokenizer);

    const char* arg = GET_STRING_RELOPTION(o, tokenizer_arg);

    if (tok && (strcmp(tok, "icu") == 0 || strcmp(tok, "splitByRegexp") == 0)) {
        if (!arg) {
            ereport(
                ERROR,
                errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("tokenizer '%s' requires tokenizer_arg", tok),
                errhint(
                    "A locale such as 'en' for icu, a regular expression for "
                    "splitByRegexp."
                )
            );
        }
    } else if (arg && !(tok && strcmp(tok, "splitByString") == 0)) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("tokenizer_arg applies only to icu, splitByRegexp and splitByString")
        );
    }
    if (o->ngram_size && (!tok || strcmp(tok, "ngrams") != 0)) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("ngram_size requires tokenizer = 'ngrams'")
        );
    }
}

/* Support function 1 of text_ops: declares the per-column options. */
PG_FUNCTION_INFO_V1(chdb_search_text_options);
Datum
chdb_search_text_options(PG_FUNCTION_ARGS) {
    local_relopts* relopts = (local_relopts*)PG_GETARG_POINTER(0);

    init_local_reloptions(relopts, sizeof(ChdbTextOptions));
    add_local_string_reloption(
        relopts,
        "tokenizer",
        "Tokenizer of the text index",
        DEFAULT_TOKENIZER,
        validate_tokenizer,
        NULL,
        offsetof(ChdbTextOptions, tokenizer)
    );
    add_local_string_reloption(
        relopts,
        "tokenizer_arg",
        "Argument of the tokenizer: icu locale, regular expression, or separator "
        "characters",
        NULL,
        NULL,
        NULL,
        offsetof(ChdbTextOptions, tokenizer_arg)
    );
    add_local_string_reloption(
        relopts,
        "preprocessor",
        "Function applied to the text before tokenizing",
        DEFAULT_PREPROCESSOR,
        validate_preprocessor,
        NULL,
        offsetof(ChdbTextOptions, preprocessor)
    );
    add_local_string_reloption(
        relopts,
        "raw_preprocessor",
        "ClickHouse expression used instead of preprocessor, superusers only",
        NULL,
        validate_raw_preprocessor,
        NULL,
        offsetof(ChdbTextOptions, raw_preprocessor)
    );
    add_local_int_reloption(
        relopts,
        "ngram_size",
        "Gram length for tokenizer = 'ngrams'",
        0,
        0,
        8,
        offsetof(ChdbTextOptions, ngram_size)
    );
    add_local_bool_reloption(
        relopts,
        "support_phrase_search",
        "Build the index so that has_phrase can use it",
        false,
        offsetof(ChdbTextOptions, support_phrase_search)
    );
    register_reloptions_validator(relopts, validate_text_options);
    PG_RETURN_VOID();
}

/*
 * Support function 1 of the classes without options: declares none. The
 * access method tells a column's kind by which of these procs its class
 * names (ddl.c), so text_array_ops has one of its own.
 */
PG_FUNCTION_INFO_V1(chdb_search_no_options);
Datum
chdb_search_no_options(PG_FUNCTION_ARGS) {
    /* Just the varlena header: a zero-size options struct breaks
     * CopyIndexAttOptions. */
    init_local_reloptions((local_relopts*)PG_GETARG_POINTER(0), sizeof(int32));
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(chdb_search_text_array_options);
Datum
chdb_search_text_array_options(PG_FUNCTION_ARGS) {
    return chdb_search_no_options(fcinfo);
}
