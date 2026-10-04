#ifndef CHDB_SEARCH_PAGESTORE_DIR_H
#define CHDB_SEARCH_PAGESTORE_DIR_H

/*
 * The blob directory of a chdb index relation (dir.c): keys to entries.
 * Entries returned are copies, palloc'd in the caller's context.
 */

#include "postgres.h"

#include "pages.h"

/* The entry for `key`, if there is one. */
extern bool
chdb_dir_lookup(ChdbPages* p, const char* key, ChdbDirEntry** entry);

/*
 * Adds the entry, of `len` bytes, replacing the key's current one in the
 * same record, which is returned for the caller to release the pages of;
 * NULL when the key was new.
 */
extern ChdbDirEntry*
chdb_dir_put(ChdbPages* p, const ChdbDirEntry* e, Size len);

/* Removes the key's entry and returns it, or NULL for a key not there. */
extern ChdbDirEntry*
chdb_dir_remove(ChdbPages* p, const char* key);

/* Called for every entry whose key starts with the prefix, under a buffer lock. */
typedef void (*ChdbDirSink)(void* ud, const ChdbDirEntry* e);

extern void
chdb_dir_list(ChdbPages* p, const char* prefix, ChdbDirSink sink, void* ud);

/* Whether any entry's key starts with the prefix; stops at the first. */
extern bool
chdb_dir_any(ChdbPages* p, const char* prefix);

/*
 * The latest LSN among the metapage and the directory pages: a version of
 * the directory, since every entry put or removed moves one of them.
 */
extern XLogRecPtr
chdb_dir_lsn(ChdbPages* p);

#endif /* CHDB_SEARCH_PAGESTORE_DIR_H */
