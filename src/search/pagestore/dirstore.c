/*
 * The directory blob store: the engine's blobs as files under the data
 * directory, laid out as dirpath.c says, so that the page request protocol
 * can be proved before the page format exists. Not the destination: a blob
 * here is no more replicated than the Phase 0 store was, and the fail-safe
 * check of meta.c still guards it.
 *
 * A pending write goes to a temporary file outside every storage, so that no
 * listing sees it; its commit fsyncs the file and renames it over whatever
 * the key held (durable_rename), so a blob is whole or absent after a crash,
 * and its mtime is that of its last byte. Pending writes a crash left are
 * cleared when the worker starts. See store.h.
 */

#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage/copydir.h"
#include "storage/fd.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#include "dirpath.h"
#include "store.h"

/* A pending write, in TopMemoryContext until its commit or abort. */
typedef struct Pending {
    File file;  /* the temporary file, -1 once closed */
    off_t size; /* bytes appended so far */
    char* tmp;  /* its path */
    char* path; /* where the commit puts it */
} Pending;

static void
dir_init(void) {
    char* tmp = dirpath_tmp_dir();
    DIR* dir  = AllocateDir(tmp);
    struct dirent* de;

    if (!dir && errno == ENOENT) {
        return;
    }
    while ((de = ReadDir(dir, tmp)) != NULL) {
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
            unlink(psprintf("%s/%s", tmp, de->d_name));
        }
    }
    FreeDir(dir);
}

static void
dir_storages(chdbBlobNameSink sink, void* ud) {
    char* root = dirpath_blobs();
    DIR* dir   = AllocateDir(root);
    struct dirent* de;
    struct stat st;

    if (!dir && errno == ENOENT) {
        return;
    }
    while ((de = ReadDir(dir, root)) != NULL) {
        if (de->d_name[0] != '.' &&
            stat(psprintf("%s/%s", root, de->d_name), &st) == 0 &&
            S_ISDIR(st.st_mode)) {
            sink(ud, de->d_name);
        }
    }
    FreeDir(dir);
}

/* stat of a blob: false when there is none, raising on any other failure. */
static bool
blob_stat(const char* storage, const char* key, struct stat* st) {
    char* path = dirpath_key(storage, key);

    if (stat(path, st) == 0) {
        return S_ISREG(st->st_mode);
    }
    if (errno != ENOENT) {
        ereport(
            ERROR,
            errcode_for_file_access(),
            errmsg("chdb_search: could not stat blob \"%s\": %m", key)
        );
    }
    return false;
}

static bool
dir_exists(const char* storage, const char* key) {
    struct stat st;

    return blob_stat(storage, key, &st);
}

static bool
dir_metadata(const char* storage, const char* key, uint64* size, int64* mtime) {
    struct stat st;

    if (!blob_stat(storage, key, &st)) {
        return false;
    }
    *size  = (uint64)st.st_size;
    *mtime = (int64)st.st_mtime;
    return true;
}

static size_t
dir_read(const char* storage, const char* key, uint64 offset, void* buf, size_t len) {
    char* path = dirpath_key(storage, key);
    int fd     = OpenTransientFile(path, O_RDONLY | PG_BINARY);
    size_t got = 0;
    int err    = 0;

    if (fd < 0) {
        ereport(
            ERROR,
            errcode_for_file_access(),
            errmsg("chdb_search: could not open blob \"%s\": %m", key)
        );
    }
    while (got < len) {
        ssize_t n = pread(fd, (char*)buf + got, len - got, (off_t)(offset + got));

        if (n > 0) {
            got += (size_t)n;
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            err = errno;
            break;
        }
    }
    CloseTransientFile(fd);
    if (err) {
        errno = err;
        ereport(
            ERROR,
            errcode_for_file_access(),
            errmsg("chdb_search: could not read blob \"%s\": %m", key)
        );
    }
    return got;
}

static void
free_pending(Pending* p) {
    if (p->file >= 0) {
        FileClose(p->file);
    }
    pfree(p->tmp);
    pfree(p->path);
    pfree(p);
}

static void*
dir_write_begin(const char* storage, const char* key) {
    char* path = dirpath_key(storage, key);
    char* tmp  = dirpath_tmp();
    Pending* p = MemoryContextAllocZero(TopMemoryContext, sizeof(*p));

    p->path = MemoryContextStrdup(TopMemoryContext, path);
    p->tmp  = MemoryContextStrdup(TopMemoryContext, tmp);
    p->file = PathNameOpenFile(p->tmp, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
    if (p->file < 0) {
        int err = errno;

        free_pending(p);
        errno = err;
        ereport(
            ERROR,
            errcode_for_file_access(),
            errmsg("chdb_search: could not create pending blob \"%s\": %m", key)
        );
    }
    return p;
}

static void
dir_write_append(void* handle, const void* buf, size_t len) {
    Pending* p = handle;

    while (len) {
        ssize_t n = FileWrite(p->file, buf, len, p->size, PG_WAIT_EXTENSION);

        if (n <= 0) {
            ereport(
                ERROR,
                errcode_for_file_access(),
                errmsg("chdb_search: could not write blob \"%s\": %m", p->path)
            );
        }
        p->size += n;
        buf = (const char*)buf + n;
        len -= (size_t)n;
    }
}

static void
dir_write_abort(void* handle) {
    Pending* p = handle;

    unlink(p->tmp);
    free_pending(p);
}

/* The rename fsyncs the file and the directory; a failure drops the pending blob. */
static void
dir_write_commit(void* handle) {
    Pending* p = handle;

    FileClose(p->file);
    p->file = -1;
    PG_TRY();
    {
        dirpath_make_parent(p->path);
        durable_rename(p->tmp, p->path, ERROR);
    }
    PG_CATCH();
    {
        dir_write_abort(p);
        PG_RE_THROW();
    }
    PG_END_TRY();
    free_pending(p);
}

/* The directories the key made empty go with it, the storage's own included. */
static void
dir_remove(const char* storage, const char* key) {
    char* path  = dirpath_key(storage, key);
    size_t stop = strlen(dirpath_blobs()) + 1;

    if (unlink(path) < 0 && errno != ENOENT) {
        ereport(
            ERROR,
            errcode_for_file_access(),
            errmsg("chdb_search: could not remove blob \"%s\": %m", key)
        );
    }
    for (char* slash = strrchr(path, '/'); slash && (size_t)(slash - path) > stop;
         slash       = strrchr(path, '/')) {
        *slash = '\0';
        if (rmdir(path) < 0) {
            break;
        }
    }
}

/* Every regular file under base whose key, the path past rootlen, starts with prefix.
 */
static void
walk(
    const char* base,
    size_t rootlen,
    const char* prefix,
    chdbBlobListSink sink,
    void* ud
) {
    DIR* dir = AllocateDir(base);
    struct dirent* de;
    struct stat st;

    if (!dir && errno == ENOENT) {
        return;
    }
    while ((de = ReadDir(dir, base)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }

        char* path = psprintf("%s/%s", base, de->d_name);

        if (lstat(path, &st) < 0) {
            continue; /* gone meanwhile */
        }
        if (S_ISDIR(st.st_mode)) {
            walk(path, rootlen, prefix, sink, ud);
        } else if (
            S_ISREG(st.st_mode) && strncmp(path + rootlen, prefix, strlen(prefix)) == 0
        ) {
            sink(ud, path + rootlen, (uint64)st.st_size, (int64)st.st_mtime);
        }
        pfree(path);
    }
    FreeDir(dir);
}

/* Walks from the deepest directory the prefix names whole. */
static void
dir_list(const char* storage, const char* prefix, chdbBlobListSink sink, void* ud) {
    char* root        = dirpath_storage(storage);
    const char* slash = strrchr(prefix, '/');

    dirpath_check_prefix(prefix);
    walk(
        slash ? psprintf("%s/%.*s", root, (int)(slash - prefix), prefix) : root,
        strlen(root) + 1,
        prefix,
        sink,
        ud
    );
}

static void
dir_copy(const char* storage, const char* from, const char* to) {
    char* src = dirpath_key(storage, from);
    char* dst = dirpath_key(storage, to);
    char* tmp = dirpath_tmp();

    PG_TRY();
    {
        copy_file(src, tmp);
        dirpath_make_parent(dst);
        durable_rename(tmp, dst, ERROR);
    }
    PG_CATCH();
    {
        unlink(tmp);
        PG_RE_THROW();
    }
    PG_END_TRY();
}

const chdbBlobStore chdb_blob_dirstore = {
    .init         = dir_init,
    .storages     = dir_storages,
    .exists       = dir_exists,
    .metadata     = dir_metadata,
    .read         = dir_read,
    .write_begin  = dir_write_begin,
    .write_append = dir_write_append,
    .write_commit = dir_write_commit,
    .write_abort  = dir_write_abort,
    .remove       = dir_remove,
    .list         = dir_list,
    .copy         = dir_copy,
};
