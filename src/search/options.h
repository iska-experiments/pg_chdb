#ifndef CHDB_SEARCH_OPTIONS_H
#define CHDB_SEARCH_OPTIONS_H

/*
 * The per-column options of the text_ops operator class, declared by
 * options.c (the options support function) and rendered into ClickHouse
 * DDL by textindex.c.
 */

#include "postgres.h"

#include "access/reloptions.h"

typedef struct ChdbTextOptions {
    int32 vl_len_;
    int tokenizer; /* string offsets, zero for unset */
    int tokenizer_arg;
    int preprocessor;
    int raw_preprocessor;
    int ngram_size; /* zero for unset */
    bool support_phrase_search;
} ChdbTextOptions;

#define DEFAULT_TOKENIZER "splitByNonAlpha"
#define DEFAULT_PREPROCESSOR "lowerUTF8"
#define DEFAULT_NGRAM_SIZE 3

#endif /* CHDB_SEARCH_OPTIONS_H */
