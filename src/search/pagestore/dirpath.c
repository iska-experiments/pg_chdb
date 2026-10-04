/*
 * Where the directory blob store puts a blob: pg_chdb/<dboid>/blobs/ holds a
 * directory per storage, <storage>/<key> with the key's slashes separating
 * directories, and .tmp/<n> for the pending writes, outside every storage.
 * Keys and storage names are checked here to stay inside their directory,
 * since they come from the engine as libchdb chose them. See dirpath.h.
 */

#include "postgres.h"

#include <ctype.h>

#include "common/file_perm.h"
#include "miscadmin.h"

#include "../protocol.h"
#include "dirpath.h"
#include "protocol.h"

#define TMP_DIR ".tmp"

static uint64 next_tmp;

char*
dirpath_blobs(void) {
    return psprintf(CHDB_SEARCH_DIR "/%u/blobs", MyDatabaseId);
}

static void
check_storage(const char* storage) {
    size_t len = strlen(storage);
    bool ok    = len > 0 && len < CHDB_PAGE_STORAGE_MAX;

    for (size_t i = 0; ok && i < len; i++) {
        ok = isalnum((unsigned char)storage[i]) || storage[i] == '_';
    }
    if (!ok) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("chdb_search: invalid blob storage name \"%s\"", storage)
        );
    }
}

/*
 * Inside its storage: no "." or ".." component, no leading slash, no empty
 * component. A prefix may be empty or end in a slash; a key may not.
 */
static void
check_path(const char* path, bool prefix) {
    const char* at = path;
    bool ok        = prefix || *path != '\0';

    while (ok && *at) {
        const char* end = strchr(at, '/');
        size_t n        = end ? (size_t)(end - at) : strlen(at);

        ok = n > 0 && strncmp(at, ".", n) != 0 && strncmp(at, "..", n) != 0;
        at = end ? end + 1 : at + n;
        if (end && !*at && !prefix) {
            ok = false;
        }
    }
    if (!ok) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg(
                "chdb_search: invalid blob %s \"%s\"", prefix ? "prefix" : "key", path
            )
        );
    }
}

char*
dirpath_storage(const char* storage) {
    check_storage(storage);
    return psprintf("%s/%s", dirpath_blobs(), storage);
}

char*
dirpath_key(const char* storage, const char* key) {
    check_path(key, false);

    char* path = psprintf("%s/%s", dirpath_storage(storage), key);

    if (strlen(path) >= MAXPGPATH) {
        ereport(
            ERROR,
            errcode(ERRCODE_NAME_TOO_LONG),
            errmsg("chdb_search: blob key \"%s\" is too long", key)
        );
    }
    return path;
}

void
dirpath_make_parent(const char* path) {
    char* parent = pstrdup(path);

    *strrchr(parent, '/') = '\0';
    if (pg_mkdir_p(parent, pg_dir_create_mode) < 0) {
        ereport(
            ERROR,
            errcode_for_file_access(),
            errmsg("chdb_search: could not create directory \"%s\": %m", parent)
        );
    }
}

char*
dirpath_tmp(void) {
    char* path = psprintf("%s/" UINT64_FORMAT, dirpath_tmp_dir(), ++next_tmp);

    dirpath_make_parent(path);
    return path;
}

char*
dirpath_tmp_dir(void) {
    return psprintf("%s/" TMP_DIR, dirpath_blobs());
}

void
dirpath_check_prefix(const char* prefix) {
    check_path(prefix, true);
}
