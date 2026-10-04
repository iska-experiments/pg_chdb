/*
 * Writing a blob into the pages of a chdb index relation. A write buffers
 * what it is given until the blob is too big to stay in its directory
 * entry, then writes full pages as they fill, each its own WAL record, and
 * keeps their numbers; its commit writes the last page and the map chain,
 * then the directory entry, which is what makes the blob visible, and gives
 * back the pages of the entry it replaced (chain.c). Nothing is flushed
 * here: the commit record of the transaction that asked for the write
 * follows these records in the WAL, so the blob is durable in commit order,
 * which is what the engine asks of a host (chdb.h). See blob.h.
 */

#include "postgres.h"

#include <time.h>

#include "utils/memutils.h"

#include "blob.h"
#include "dir.h"

struct ChdbBlobWrite {
    RelFileLocator loc;
    char* key;
    uint64 size;         /* bytes appended so far */
    char* buf;           /* bytes not yet in a page */
    Size nbuf, cap;      /* used and allocated */
    BlockNumber* blocks; /* the data pages written, in order */
    uint32 nblocks, bcap;
};

static void
check_key(const char* key) {
    size_t len = strlen(key);

    if (len == 0 || len > CHDB_KEY_MAX) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("chdb_search: invalid blob key \"%.80s%s\"", key, len > 80 ? "..." : "")
        );
    }
}

/* Opens the relation of a write, raising if it is gone. */
static void
open_or_raise(ChdbPages* p, RelFileLocator loc) {
    if (!chdb_pages_open(p, loc, NULL)) {
        ereport(
            ERROR,
            errcode(ERRCODE_UNDEFINED_OBJECT),
            errmsg("chdb_search: the index of relation %u is gone", loc.relNumber)
        );
    }
}

/* ---- writing ---- */

ChdbBlobWrite*
chdb_blob_begin(RelFileLocator loc, const char* key) {
    ChdbPages p;

    check_key(key);
    open_or_raise(&p, loc);

    ChdbBlobWrite* w = MemoryContextAllocZero(TopMemoryContext, sizeof(*w));

    w->loc = loc;
    w->key = MemoryContextStrdup(TopMemoryContext, key);
    return w;
}

/* Writes one data page of `len` bytes and remembers its number. */
static void
write_page(ChdbBlobWrite* w, ChdbPages* p, const char* data, Size len) {
    ChdbWrite x;
    BlockNumber blk;

    chdb_write_begin(&x, p);

    Page page = chdb_write_alloc(&x, CHDB_PAGE_DATA, &blk);

    memcpy((char*)page + CHDB_PAGE_START, data, len);
    ((PageHeader)page)->pd_lower = CHDB_PAGE_START + len;
    chdb_write_finish(&x);

    if (w->nblocks == w->bcap) {
        w->bcap   = w->bcap ? w->bcap * 2 : 64;
        w->blocks = w->blocks ? repalloc(w->blocks, w->bcap * sizeof(BlockNumber))
                              : MemoryContextAlloc(
                                    TopMemoryContext, w->bcap * sizeof(BlockNumber)
                                );
    }
    w->blocks[w->nblocks++] = blk;
}

/* Writes the full pages the buffer holds; with `all`, the partial last one too. */
static void
drain(ChdbBlobWrite* w, ChdbPages* p, bool all) {
    Size at = 0;

    while (w->nbuf - at >= CHDB_DATA_PER_PAGE || (all && w->nbuf > at)) {
        Size n = Min(CHDB_DATA_PER_PAGE, w->nbuf - at);

        write_page(w, p, w->buf + at, n);
        at += n;
    }
    memmove(w->buf, w->buf + at, w->nbuf - at);
    w->nbuf -= at;
}

void
chdb_blob_append(ChdbBlobWrite* w, const void* buf, Size len) {
    ChdbPages p;

    if (w->nbuf + len > w->cap) {
        w->cap = Max(w->nbuf + len, Max(2 * w->cap, (Size)CHDB_INLINE_MAX + 1));
        w->buf = w->buf ? repalloc(w->buf, w->cap)
                        : MemoryContextAlloc(TopMemoryContext, w->cap);
    }
    memcpy(w->buf + w->nbuf, buf, len);
    w->nbuf += len;
    w->size += len;
    /* Small enough to be inline so far: keep it, the entry may take it whole. */
    if (w->size > CHDB_INLINE_MAX && w->nbuf >= CHDB_DATA_PER_PAGE) {
        open_or_raise(&p, w->loc);
        drain(w, &p, false);
    }
}

/* Writes the map chain over the data pages, last page first, and returns its head. */
static BlockNumber
write_map(ChdbPages* p, const BlockNumber* blocks, uint32 n) {
    BlockNumber next = InvalidBlockNumber;

    for (uint32 i = (n + CHDB_LIST_PER_PAGE - 1) / CHDB_LIST_PER_PAGE; i-- > 0;) {
        uint32 from  = i * CHDB_LIST_PER_PAGE;
        uint32 count = Min(CHDB_LIST_PER_PAGE, n - from);
        ChdbWrite x;
        BlockNumber blk;

        chdb_write_begin(&x, p);

        Page page = chdb_write_alloc(&x, CHDB_PAGE_MAP, &blk);

        CHDB_SPECIAL(page)->next = next;
        CHDB_LIST(page)->refs    = i == 0 ? 1 : 0;
        CHDB_LIST(page)->count   = count;
        memcpy(CHDB_LIST_BLOCKS(page), blocks + from, count * sizeof(BlockNumber));
        ((PageHeader)page)->pd_lower =
            CHDB_PAGE_START + sizeof(ChdbListHeader) + count * sizeof(BlockNumber);
        chdb_write_finish(&x);
        next = blk;
    }
    return next;
}

static void
free_write(ChdbBlobWrite* w) {
    if (w->buf) {
        pfree(w->buf);
    }
    if (w->blocks) {
        pfree(w->blocks);
    }
    pfree(w->key);
    pfree(w);
}

static void
commit(ChdbBlobWrite* w) {
    ChdbPages p;
    Size keylen = strlen(w->key);
    bool inl    = w->size <= CHDB_INLINE_MAX;
    Size len    = CHDB_DIR_ENTRY_SIZE(keylen, inl ? w->size : 0);
    ChdbDirEntry* e;

    open_or_raise(&p, w->loc);
    if (!inl) {
        drain(w, &p, true);
    }
    e         = palloc(len);
    e->size   = w->size;
    e->mtime  = (int64)time(NULL);
    e->map    = inl ? InvalidBlockNumber : write_map(&p, w->blocks, w->nblocks);
    e->keylen = (uint16)keylen;
    memcpy(e->key, w->key, keylen);
    if (inl) {
        memcpy(CHDB_DIR_ENTRY_DATA(e), w->buf, w->size);
    }

    ChdbDirEntry* old = chdb_dir_put(&p, e, len);

    /* Published: the pages are the directory's now, not this write's to free. */
    w->nblocks = 0;
    if (old) {
        chdb_blob_release(&p, old);
    }
}

void
chdb_blob_commit(ChdbBlobWrite* w) {
    PG_TRY();
    { commit(w); }
    PG_CATCH();
    {
        chdb_blob_abort(w);
        PG_RE_THROW();
    }
    PG_END_TRY();
    free_write(w);
}

void
chdb_blob_abort(ChdbBlobWrite* w) {
    ChdbPages p;

    PG_TRY();
    {
        if (w->nblocks && chdb_pages_open(&p, w->loc, NULL)) {
            chdb_pages_free(&p, w->blocks, w->nblocks);
        }
    }
    PG_CATCH();
    {
        /* A relation gone from under the write has nothing to give back. */
        FlushErrorState();
    }
    PG_END_TRY();
    free_write(w);
}
