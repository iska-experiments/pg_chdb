#ifndef CHDB_SEARCH_PAGESTORE_DIRPATH_H
#define CHDB_SEARCH_PAGESTORE_DIRPATH_H

/*
 * The paths of the directory blob store, each checked and palloc'd in the
 * current memory context. See dirpath.c for the layout.
 */

#include "postgres.h"

/* pg_chdb/<dboid>/blobs, for the current database. */
extern char*
dirpath_blobs(void);

/* The directory of a storage, whose name is checked. */
extern char*
dirpath_storage(const char* storage);

/* The file of a blob, whose key is checked. */
extern char*
dirpath_key(const char* storage, const char* key);

/* The directory of the pending writes, and a fresh file in it, its directory made. */
extern char*
dirpath_tmp_dir(void);
extern char*
dirpath_tmp(void);

/* Makes the directory `path` is in, with its parents. */
extern void
dirpath_make_parent(const char* path);

/* Raises for a listing prefix that would leave its storage. */
extern void
dirpath_check_prefix(const char* prefix);

#endif /* CHDB_SEARCH_PAGESTORE_DIRPATH_H */
