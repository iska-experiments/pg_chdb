#ifndef CHDB_SEARCH_PAGESTORE_STORE_H
#define CHDB_SEARCH_PAGESTORE_STORE_H

/*
 * Where the worker keeps the engine's blobs: the backend behind the page
 * requests of protocol.h, which dispatch.c answers by calling these. One
 * backend is linked at a time. The directory store (dirstore.c) keeps blobs
 * as files under the data directory, so that the protocol stands before the
 * page format does; the next backend keeps them in pages of the index
 * relation, written with generic WAL.
 *
 * A storage is one of the engine's callback object storages, named
 * pg_<indexoid> for the index whose tables it holds (CHDB_STORE_STORAGE_FMT
 * in ../protocol.h); a key is the '/'-separated path libchdb chose, opaque
 * to the host. Every function but write_abort raises on failure, and the
 * dispatcher turns the error into the reply; write_abort never raises, as
 * it runs on the failure paths. Handles are the backend's own, allocated to
 * live until their commit or abort, which free them.
 */

#include "postgres.h"

typedef void (*chdbBlobNameSink)(void* ud, const char* name);
typedef void (*chdbBlobListSink)(void* ud, const char* key, uint64 size, int64 mtime);

typedef struct chdbBlobStore {
    /* Once per worker, before the first request: clears what a crash left. */
    void (*init)(void);
    /* Names every storage that holds blobs, in any order. */
    void (*storages)(chdbBlobNameSink sink, void* ud);
    bool (*exists)(const char* storage, const char* key);
    /* False for a blob that is not there; size and mtime, Unix seconds, of one that is.
     */
    bool (*metadata)(const char* storage, const char* key, uint64* size, int64* mtime);
    /* min(len, size - offset) bytes of the blob into buf; a missing blob raises. */
    size_t (*read)(
        const char* storage,
        const char* key,
        uint64 offset,
        void* buf,
        size_t len
    );
    /* A pending blob, invisible until committed, replacing any blob at the key then. */
    void* (*write_begin)(const char* storage, const char* key);
    void (*write_append)(void* handle, const void* buf, size_t len);
    /* Durable before it returns. Frees the handle whether or not it raises. */
    void (*write_commit)(void* handle);
    void (*write_abort)(void* handle);
    /* Removing a missing blob is success. */
    void (*remove)(const char* storage, const char* key);
    /* Every blob whose key starts with prefix, in any order. */
    void (*list)(
        const char* storage,
        const char* prefix,
        chdbBlobListSink sink,
        void* ud
    );
    /* Replaces to with a copy of from; a missing from raises. */
    void (*copy)(const char* storage, const char* from, const char* to);
    /*
     * Drops a storage whole, once the engine has dropped the index's database:
     * what is left in it, half-written parts of a crashed engine say, is
     * garbage the engine never reclaims. Nothing to drop is success.
     */
    void (*remove_storage)(const char* storage);
} chdbBlobStore;

/* dirstore.c: files under pg_chdb/<dboid>/blobs/<storage>/<key>. */
extern const chdbBlobStore chdb_blob_dirstore;

#endif /* CHDB_SEARCH_PAGESTORE_STORE_H */
