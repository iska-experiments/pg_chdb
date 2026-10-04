/*
 * The URLs a COPY or CREATE TABLE names, taken apart the way the ClickHouse
 * table functions want their arguments: an Azure URL into account, container
 * and path, a file URL into its local path.
 */

#include "postgres.h"

#include "url.h"

/*
* Decomposition of an Azure URL into the arguments the `azureBlobStorage`
  engine expects.
*/
void
chdb_parse_azure_url(chdbCopyContext* ctx, chdbAzureURLParts* parts) {
    /*
     * Based on Azure URL parsing for the url() function in ClickHouse 26.7:
     * https://github.com/ClickHouse/ClickHouse/blob/0b235b0/src/Storages/StorageURL.cpp#L2016-L2087
     */
    char* uri = strstr(ctx->url, "://");
    if (!uri) {
        /* Should not happen, validated by the hook. */
        elog(ERROR, "chdb: malformed Azure URL %s", ctx->url);
    }

    uri += 3;

    /*
     * Split off the query string (a SAS token such as `?sp=...&sig=...`)
     * before parsing the host and path.
     */
    char* query = strchr(uri, '?');
    if (query) {
        /* NUL terminate the URL and split off the query. */
        *query = '\0';
        query++;
    }

    /*
     * Hadoop-style
     * `abfss://<container>@<account>.dfs.core.windows.net/<blob path>`.
     */
    if (ctx->scheme == abfs_scheme) {
        char* at = strchr(uri, '@');
        if (!at) {
            ereport(
                ERROR,
                errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("chdb: Azure ABFS URL missing the container part"),
                errhint("abfs://<container>@<account>.dfs.core.windows.net/<path>")
            );
        }

        parts->container    = pnstrdup(uri, at - uri);
        char* host_and_path = at + 1;
        char* slash         = strchr(host_and_path, '/');
        char* host =
            slash ? pnstrdup(host_and_path, slash - host_and_path) : host_and_path;
        parts->path        = slash ? slash + 1 : "";
        char* dot          = strchr(host, '.');
        char* account      = dot ? host : psprintf("%s.blob.core.windows.net", host);
        parts->account_url = query ? psprintf("https://%s?%s", account, query)
                                   : psprintf("https://%s", account);
        return;
    }

    /*
     * `<account>.blob.core.windows.net/<container>/<blob>` or
     * `<host>/<container>/<blob>`.
     */
    char* path = strstr(uri, "/");

    /*
     * `az://<account>.blob.core.windows.net/<container>/<blob>` or
     * `azure://<host>/<container>/<blob>`
     */
    const char* host = path ? pnstrdup(uri, path - uri) : uri;
    path             = path ? path + 1 : "";

    const char* dot = strchr(host, '.');
    if (!dot) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("chdb: Azure URL missing the storage account host"),
            errhint("az://<account>.blob.core.windows.net/<container>/<path>")
        );
    }

    parts->account_url =
        query ? psprintf("https://%s?%s", host, query) : psprintf("https://%s", host);
    char* slash = strchr(path, '/');
    parts->container =
        slash ? pnstrdup(path, slash - path) : pnstrdup(path, strlen(path));
    parts->path = slash ? slash + 1 : "";

    if (strlen(parts->container) == 0) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("chdb: Azure URL missing the container name"),
            errhint("az://<account>.blob.core.windows.net/<container>/<path>")
        );
    }
}

/*
 * Get the local path from a file:// URL. Must be an absolute path or else it
 * raises an error.
 */
char*
chdb_file_url_path(const char* url) {
    /* https://github.com/ClickHouse/ClickHouse/blob/0b235b0/src/Storages/StorageURL.cpp#L2000-L2014*/
    char* path = (char*)strstr(url, "://"); /* borrowed from url */
    if (!path) {
        /* Should not happen, validated by the hook. */
        elog(ERROR, "chdb: malformed file URL %s", url);
    }

    path += 3;
    if (!is_absolute_path(path)) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_NAME),
            errmsg("chdb: relative path not allowed for COPY to file URL")
        );
    }

    return path;
}
