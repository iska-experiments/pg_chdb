#ifndef CHDB_URL_H
#define CHDB_URL_H

#include "postgres.h"

#include "copy.h"

/*
 * Decomposition of an Azure URL into the arguments that `azureBlobStorage()`
 * expects.
 */
typedef struct chdbAzureURLParts {
    char* account_url;
    char* container;
    char* path;
} chdbAzureURLParts;

/* Parses `ctx->url`, an az:// or abfs:// URL, into `parts`. */
extern void
chdb_parse_azure_url(chdbCopyContext* ctx, chdbAzureURLParts* parts);

/* The local path of a file:// URL, which must be absolute. Borrowed from `url`. */
extern char*
chdb_file_url_path(const char* url);

#endif /* CHDB_URL_H */
