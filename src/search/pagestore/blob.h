#ifndef CHDB_SEARCH_PAGESTORE_BLOB_H
#define CHDB_SEARCH_PAGESTORE_BLOB_H

/*
 * The bytes of a blob (blob.c): read out of its pages, written into pages
 * through a pending write that its commit publishes, and the pages given
 * back when the last directory entry naming them goes. See pages.h.
 */

#include "postgres.h"

#include "pages.h"

/* A write under way, in TopMemoryContext until its commit or abort. */
typedef struct ChdbBlobWrite ChdbBlobWrite;

/*
 * Never raises for a relation that is gone, or an invalid `loc`: such a
 * write is taken and its bytes dropped, as the engine's drop of a table
 * whose pages went with its index must not fail.
 */
extern ChdbBlobWrite*
chdb_blob_begin(RelFileLocator loc, const char* key);
extern void
chdb_blob_append(ChdbBlobWrite* w, const void* buf, Size len);
/* Publishes the blob under its key, replacing any there. Frees w even on raise. */
extern void
chdb_blob_commit(ChdbBlobWrite* w);
/* Gives the pages back and frees w, the latter even on a raise. */
extern void
chdb_blob_abort(ChdbBlobWrite* w);

/* min(len, size - offset) bytes of the blob into buf. */
extern Size
chdb_blob_read(ChdbPages* p, const ChdbDirEntry* e, uint64 offset, void* buf, Size len);

/* One more directory entry shares the blob's pages: before a copy's entry is put. */
extern void
chdb_blob_share(ChdbPages* p, const ChdbDirEntry* e);
/* One entry fewer; the pages go back to the free stack with the last. */
extern void
chdb_blob_release(ChdbPages* p, const ChdbDirEntry* e);

#endif /* CHDB_SEARCH_PAGESTORE_BLOB_H */
