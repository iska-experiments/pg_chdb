/*
 * The blob store on index pages: store.h's table of functions, each finding
 * the relation behind a storage and a key (routes.c) and working its
 * directory (dir.c) and pages (blob.c, chain.c). A relation the worker was
 * never told of, or that is gone, is a storage without blobs: nothing is
 * found in it, removing from it and copying within it succeed, a write
 * into it is taken and discarded, and only a read raises. Before a
 * relation is given up as gone the catalog is asked where the generation
 * is now (routes.h), so a write never goes to the void while its pages
 * live elsewhere.
 */

#include "postgres.h"

#include <time.h>

#include "blob.h"
#include "dir.h"
#include "pagestore.h"
#include "routes.h"
#include "store.h"

/*
 * The relation for a storage and a key, open: the one routed, or, with
 * that one gone, the one the catalog's index holds the generation in now.
 * False leaves `loc` as the last route tried, invalid when there was none.
 */
static bool
route(const char* storage, const char* key, RelFileLocator* loc, ChdbPages* p) {
    loc->relNumber = InvalidRelFileNumber;
    if (chdb_routes_find(storage, key, loc) && chdb_pages_open(p, *loc, NULL)) {
        return true;
    }
    return chdb_routes_refresh(storage, key, loc) && chdb_pages_open(p, *loc, NULL);
}

static bool
open_for(const char* storage, const char* key, ChdbPages* p) {
    RelFileLocator loc;

    return route(storage, key, &loc, p);
}

static bool
open_entry(const char* storage, const char* key, ChdbPages* p, ChdbDirEntry** e) {
    return open_for(storage, key, p) && chdb_dir_lookup(p, key, e);
}

static void
missing(const char* key) {
    ereport(
        ERROR,
        errcode(ERRCODE_UNDEFINED_OBJECT),
        errmsg("chdb_search: blob \"%s\" does not exist", key)
    );
}

static bool
pg_exists(const char* storage, const char* key) {
    ChdbPages p;
    ChdbDirEntry* e;

    return open_entry(storage, key, &p, &e);
}

static bool
pg_metadata(const char* storage, const char* key, uint64* size, int64* mtime) {
    ChdbPages p;
    ChdbDirEntry* e;

    if (!open_entry(storage, key, &p, &e)) {
        return false;
    }
    *size  = e->size;
    *mtime = e->mtime;
    return true;
}

static size_t
pg_read(const char* storage, const char* key, uint64 offset, void* buf, size_t len) {
    ChdbPages p;
    ChdbDirEntry* e;

    if (!open_entry(storage, key, &p, &e)) {
        missing(key);
    }
    return chdb_blob_read(&p, e, offset, buf, len);
}

/* A write into a relation that is gone is taken and discarded (blob.c). */
static void*
pg_write_begin(const char* storage, const char* key) {
    RelFileLocator loc;
    ChdbPages p;

    route(storage, key, &loc, &p);
    return chdb_blob_begin(loc, key);
}

static void
pg_write_append(void* handle, const void* buf, size_t len) {
    chdb_blob_append(handle, buf, len);
}

static void
pg_write_commit(void* handle) {
    chdb_blob_commit(handle);
}

static void
pg_write_abort(void* handle) {
    chdb_blob_abort(handle);
}

static void
pg_remove(const char* storage, const char* key) {
    ChdbPages p;

    if (open_for(storage, key, &p)) {
        ChdbDirEntry* e = chdb_dir_remove(&p, key);

        if (e) {
            chdb_blob_release(&p, e);
        }
    }
}

typedef struct Listing {
    chdbBlobListSink sink;
    void* ud;
} Listing;

static void
list_entry(void* ud, const ChdbDirEntry* e) {
    Listing* l = ud;
    char* key  = pnstrdup(e->key, e->keylen);

    l->sink(l->ud, key, e->size, e->mtime);
    pfree(key);
}

static void
pg_list(const char* storage, const char* prefix, chdbBlobListSink sink, void* ud) {
    ChdbPages p;
    Listing l = { .sink = sink, .ud = ud };

    if (open_for(storage, prefix, &p)) {
        chdb_dir_list(&p, prefix, list_entry, &l);
    }
}

/*
 * The copy shares the source's pages: one more entry names the chain. The
 * count is raised before the entry is put, so a crash between leaves a
 * count too high, which recover.c lowers, never pages freed while named.
 *
 * A storage whose relation is gone has nothing to copy and nobody to miss
 * the copy: the engine copies a blob aside before it unlinks it, and
 * renames and writes directory markers as it drops a table whose pages
 * Postgres took with the index. Any of those failing fails the drop, which
 * ClickHouse retries without end while the request waits on it.
 */
static void
pg_copy(const char* storage, const char* from, const char* to) {
    ChdbPages p;
    ChdbDirEntry* src;
    RelFileLocator dst_loc;
    Size keylen = strlen(to);

    if (!open_for(storage, from, &p)) {
        return;
    }
    if (!chdb_dir_lookup(&p, from, &src)) {
        missing(from);
    }
    if (!chdb_routes_find(storage, to, &dst_loc) ||
        !RelFileLocatorEquals(dst_loc, p.loc)) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("chdb_search: blob \"%s\" cannot be copied to another relation", from)
        );
    }

    bool inl           = CHDB_DIR_ENTRY_INLINE(src);
    Size len           = CHDB_DIR_ENTRY_SIZE(keylen, inl ? src->size : 0);
    ChdbDirEntry* dst  = palloc(len);

    dst->size   = src->size;
    dst->mtime  = (int64)time(NULL);
    dst->map    = src->map;
    dst->keylen = (uint16)keylen;
    memcpy(dst->key, to, keylen);
    if (inl) {
        memcpy(CHDB_DIR_ENTRY_DATA(dst), CHDB_DIR_ENTRY_DATA(src), src->size);
    }
    chdb_blob_share(&p, src);

    ChdbDirEntry* old = chdb_dir_put(&p, dst, len);

    if (old) {
        chdb_blob_release(&p, old);
    }
}

const chdbBlobStore chdb_blob_pagestore = {
    .init           = chdb_routes_init,
    .storages       = chdb_routes_storages,
    .exists         = pg_exists,
    .metadata       = pg_metadata,
    .read           = pg_read,
    .write_begin    = pg_write_begin,
    .write_append   = pg_write_append,
    .write_commit   = pg_write_commit,
    .write_abort    = pg_write_abort,
    .remove         = pg_remove,
    .list           = pg_list,
    .copy           = pg_copy,
};

/* ---- a backend reading its own index ---- */

void
chdb_pagestore_list_relation(Relation index, chdbBlobListSink sink, void* ud) {
    ChdbPages p;
    Listing l = { .sink = sink, .ud = ud };

    if (chdb_pages_open(&p, index->rd_locator, index)) {
        chdb_dir_list(&p, "", list_entry, &l);
    }
}

bool
chdb_pagestore_has_blobs(Relation index, const char* prefix) {
    ChdbPages p;

    return chdb_pages_open(&p, index->rd_locator, index) && chdb_dir_any(&p, prefix);
}
