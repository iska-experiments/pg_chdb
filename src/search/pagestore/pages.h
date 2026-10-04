#ifndef CHDB_SEARCH_PAGESTORE_PAGES_H
#define CHDB_SEARCH_PAGESTORE_PAGES_H

/*
 * The layout of a chdb index relation, whose pages hold the blobs of the
 * engine's store tables, and the page I/O every operation on them shares.
 *
 *   block 0        the metapage (ChdbMetaPageData): the generation and
 *                  flush position the access method keeps (meta.c), the
 *                  head of the directory chain and the head of the free
 *                  stack
 *   directory      a chain of pages of ChdbDirEntry items, each a blob's
 *                  key, size, commit time, and either its data, when small
 *                  enough to live beside the key, or the first page of its
 *                  map chain
 *   map            a chain of pages listing a blob's data pages in order,
 *                  so that a read at an offset reaches its page in as many
 *                  hops as there are map pages before it; the first page
 *                  counts the directory entries sharing the chain, as a
 *                  copy does
 *   data           raw bytes, CHDB_DATA_PER_PAGE of them per page
 *   free stack     a chain of pages listing the free pages; a page that
 *                  runs out of numbers is itself the next page handed out,
 *                  and the first page of a run being freed takes the
 *                  numbers of the rest when the top page is full, so that
 *                  freeing allocates nothing
 *
 * Every page but the metapage ends in a ChdbPageSpecial naming its kind and
 * the next page of its chain. Every write goes through generic WAL under an
 * exclusive buffer lock, so that recovery and replication come from the
 * server; an object of several pages is published by its directory entry,
 * written last, and a crash before that leaves pages no entry names, which
 * recover.c returns to the free stack when the relation is next opened
 * with CHDB_META_DIRTY set.
 *
 * The code here takes a RelFileLocator rather than a relcache entry: the
 * worker writes the pages of an index whose CREATE INDEX has not committed,
 * which no catalog shows it, so it reads buffers by locator and logs them
 * through a stand-in relation (chdb_pages_open). A backend passes its own.
 */

#include "postgres.h"

#include "access/generic_xlog.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/relfilelocator.h"
#include "utils/rel.h"

/* ---- the metapage ---- */

#define CHDB_META_MAGIC 0x43484453 /* "CHDS" */
#define CHDB_META_VERSION 2
#define CHDB_METAPAGE_BLKNO 0

/* Pages were handed out since the flag was last cleared: recover.c's cue. */
#define CHDB_META_DIRTY 1

typedef struct ChdbMetaPageData {
    uint32 magic;
    uint32 version;
    uint64 generation;     /* random per build, names the store table */
    uint64 flushed_lsn;    /* WAL position when the store was last written */
    BlockNumber dir_head;  /* first directory page, or InvalidBlockNumber */
    BlockNumber free_head; /* top page of the free stack, or InvalidBlockNumber */
    uint32 flags;          /* CHDB_META_* */
} ChdbMetaPageData;

static inline ChdbMetaPageData*
CHDB_META(Page page) {
    return (ChdbMetaPageData*)PageGetContents(page);
}

/* ---- the other pages ---- */

typedef enum ChdbPageKind {
    CHDB_PAGE_DIR = 1,
    CHDB_PAGE_MAP,
    CHDB_PAGE_DATA,
    CHDB_PAGE_FREE,
} ChdbPageKind;

typedef struct ChdbPageSpecial {
    uint16 kind; /* ChdbPageKind */
    uint16 unused;
    BlockNumber next; /* the chain's next page, or InvalidBlockNumber */
} ChdbPageSpecial;

/* The accessors are functions: the page macros evaluate their argument more than once.
 */
static inline ChdbPageSpecial*
CHDB_SPECIAL(Page page) {
    return (ChdbPageSpecial*)PageGetSpecialPointer(page);
}

#define CHDB_PAGE_START MAXALIGN(SizeOfPageHeaderData)
#define CHDB_PAGE_END (BLCKSZ - MAXALIGN(sizeof(ChdbPageSpecial)))
/* Bytes of blob data, or of block numbers after their header, on one page. */
#define CHDB_DATA_PER_PAGE (CHDB_PAGE_END - CHDB_PAGE_START)

/* A map or free stack page: a count, then that many block numbers. */
typedef struct ChdbListHeader {
    uint32 refs;  /* first map page: directory entries sharing the chain */
    uint32 count; /* block numbers on this page */
} ChdbListHeader;

static inline ChdbListHeader*
CHDB_LIST(Page page) {
    return (ChdbListHeader*)PageGetContents(page);
}

static inline BlockNumber*
CHDB_LIST_BLOCKS(Page page) {
    return (BlockNumber*)(PageGetContents(page) + sizeof(ChdbListHeader));
}
#define CHDB_LIST_PER_PAGE                                                             \
    ((CHDB_DATA_PER_PAGE - sizeof(ChdbListHeader)) / sizeof(BlockNumber))

/* A directory entry: the key, then its data when the blob is inline. */
typedef struct ChdbDirEntry {
    uint64 size;
    int64 mtime;     /* Unix seconds of the commit */
    BlockNumber map; /* first map page; InvalidBlockNumber for an inline blob */
    uint16 keylen;
    char key[FLEXIBLE_ARRAY_MEMBER];
} ChdbDirEntry;

/* A blob this small lives in its directory entry rather than in pages of its own. */
#define CHDB_INLINE_MAX 2048
/* Longer keys are refused: plain_rewritable's are under a hundred bytes. */
#define CHDB_KEY_MAX 1024

#define CHDB_DIR_ENTRY_SIZE(keylen, inline_bytes)                                      \
    (offsetof(ChdbDirEntry, key) + (keylen) + (inline_bytes))
#define CHDB_DIR_ENTRY_DATA(e) ((e)->key + (e)->keylen)
#define CHDB_DIR_ENTRY_INLINE(e) (!BlockNumberIsValid((e)->map))

/* ---- page I/O ---- */

/* A relation open for one operation on its blobs. */
typedef struct ChdbPages {
    RelFileLocator loc;
    Relation rel; /* a backend's own, or the worker's stand-in, for WAL */
    BlockNumber nblocks;
    ChdbMetaPageData meta; /* as read at open; dir_head and free_head may move since */
} ChdbPages;

/*
 * Opens the relation at `loc` for blob operations: false when the relation
 * is gone, as a dropped index's is, or empty, which the caller treats as a
 * storage without blobs. `rel` is the caller's relcache entry, or NULL for
 * the stand-in. Raises for a relation that is not a chdb index.
 */
extern bool
chdb_pages_open(ChdbPages* p, RelFileLocator loc, Relation rel);

/* Reads and locks a page; `mode` is BUFFER_LOCK_SHARE or _EXCLUSIVE. */
extern Buffer
chdb_pages_read(ChdbPages* p, BlockNumber blk, int mode);
/* The same, raising if the page is not of `kind`, as a chain's next must be. */
extern Buffer
chdb_pages_read_kind(ChdbPages* p, BlockNumber blk, int mode, ChdbPageKind kind);

/*
 * One generic WAL record over up to MAX_GENERIC_XLOG_PAGES buffers, each
 * locked exclusively from its registration to the finish. Pages taken from
 * the free stack or extended count among them.
 */
typedef struct ChdbWrite {
    ChdbPages* p;
    GenericXLogState* xlog;
    Buffer bufs[MAX_GENERIC_XLOG_PAGES];
    Page pages[MAX_GENERIC_XLOG_PAGES];
    int n;
    Page meta; /* the metapage's image once registered, else NULL */
} ChdbWrite;

extern void
chdb_write_begin(ChdbWrite* w, ChdbPages* p);
/* Registers the metapage, once. */
extern Page
chdb_write_meta(ChdbWrite* w);
/* Registers an existing page; `full` for one rewritten whole. */
extern Page
chdb_write_page(ChdbWrite* w, BlockNumber blk, bool full);
/*
 * A fresh page, initialized for `kind` with no next, taken from the free
 * stack or extended; registered whole. Marks the metapage dirty on the
 * first allocation since it was last cleared.
 */
extern Page
chdb_write_alloc(ChdbWrite* w, ChdbPageKind kind, BlockNumber* blk);
extern void
chdb_write_finish(ChdbWrite* w);
/* Drops the record and the locks; for a caller that found nothing to change. */
extern void
chdb_write_abort(ChdbWrite* w);

/* Pushes `n` pages onto the free stack, in as many records as it takes. */
extern void
chdb_pages_free(ChdbPages* p, const BlockNumber* blks, uint32 n);

/* Clears CHDB_META_DIRTY, once no write is pending; a no-op when it is clear. */
extern void
chdb_pages_clean(ChdbPages* p);

#endif /* CHDB_SEARCH_PAGESTORE_PAGES_H */
