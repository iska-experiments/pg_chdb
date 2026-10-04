/*
 * The directory of a chdb index relation's blobs: a chain of pages of
 * ChdbDirEntry items (pages.h), unsorted, read whole for a key. Every
 * callback of the engine is a round trip to the worker, and the chain is a
 * few pages in shared buffers, so the walk is not what a lookup costs. The
 * worker is the only process that changes the directory, so a walk that
 * released its locks can act on what it found; backends only read it.
 */

#include "postgres.h"

#include "storage/bufmgr.h"

#include "dir.h"

/* Calls visit for each page of the chain under a share lock, until it says stop. */
typedef bool (*PageVisitor)(void* ud, BlockNumber blk, Page page);

static void
walk_pages(ChdbPages* p, PageVisitor visit, void* ud) {
    BlockNumber blk = p->meta.dir_head;

    while (BlockNumberIsValid(blk)) {
        Buffer buf       = chdb_pages_read(p, blk, BUFFER_LOCK_SHARE);
        Page page        = BufferGetPage(buf);
        BlockNumber next = CHDB_SPECIAL(page)->next;
        bool more        = visit(ud, blk, page);

        UnlockReleaseBuffer(buf);
        if (!more) {
            return;
        }
        blk = next;
    }
}

static ChdbDirEntry*
entry_at(Page page, OffsetNumber off) {
    return (ChdbDirEntry*)PageGetItem(page, PageGetItemId(page, off));
}

static bool
entry_is(const ChdbDirEntry* e, const char* key, size_t keylen) {
    return e->keylen == keylen && memcmp(e->key, key, keylen) == 0;
}

static ChdbDirEntry*
copy_entry(Page page, OffsetNumber off) {
    Size len        = ItemIdGetLength(PageGetItemId(page, off));
    ChdbDirEntry* e = palloc(len);

    memcpy(e, entry_at(page, off), len);
    return e;
}

/* ---- finding a key ---- */

typedef struct Find {
    const char* key;
    size_t keylen;
    BlockNumber blk; /* where it was found, else InvalidBlockNumber */
    OffsetNumber off;
    ChdbDirEntry* entry; /* a copy, when wanted */
    bool copy;
} Find;

static bool
find_visitor(void* ud, BlockNumber blk, Page page) {
    Find* f          = ud;
    OffsetNumber max = PageGetMaxOffsetNumber(page);

    for (OffsetNumber off = FirstOffsetNumber; off <= max; off++) {
        if (entry_is(entry_at(page, off), f->key, f->keylen)) {
            f->blk = blk;
            f->off = off;
            if (f->copy) {
                f->entry = copy_entry(page, off);
            }
            return false;
        }
    }
    return true;
}

static bool
find(ChdbPages* p, const char* key, bool copy, Find* f) {
    *f = (Find){ .key = key, .keylen = strlen(key), .blk = InvalidBlockNumber, .copy = copy };
    walk_pages(p, find_visitor, f);
    return BlockNumberIsValid(f->blk);
}

bool
chdb_dir_lookup(ChdbPages* p, const char* key, ChdbDirEntry** entry) {
    Find f;

    if (!find(p, key, true, &f)) {
        return false;
    }
    *entry = f.entry;
    return true;
}

/* ---- changing the directory ---- */

/* What a put needs to know of the chain, found in one walk. */
typedef struct Room {
    Find old;         /* the key's current entry, if any */
    Size need;        /* MAXALIGN'd bytes of the new entry */
    BlockNumber fit;  /* the page to add to, else InvalidBlockNumber */
    BlockNumber last; /* the last page of the chain */
} Room;

/*
 * The page holding the key's old entry takes the new one if it has the room
 * once the old is gone, as the two change in one record; else the first
 * page with room.
 */
static bool
room_visitor(void* ud, BlockNumber blk, Page page) {
    Room* r = ud;

    r->last = blk;
    if (!BlockNumberIsValid(r->old.blk) && !find_visitor(&r->old, blk, page)) {
        Size oldlen = ItemIdGetLength(PageGetItemId(page, r->old.off));

        if (PageGetExactFreeSpace(page) + MAXALIGN(oldlen) >= r->need) {
            r->fit = blk;
        }
    } else if (!BlockNumberIsValid(r->fit) && PageGetFreeSpace(page) >= r->need) {
        r->fit = blk;
    }
    return true; /* to the end, for `last` */
}

/* Adds a directory page at the end of the chain and returns its number. */
static BlockNumber
extend_chain(ChdbPages* p, BlockNumber last) {
    ChdbWrite w;
    BlockNumber blk;

    chdb_write_begin(&w, p);
    chdb_write_alloc(&w, CHDB_PAGE_DIR, &blk);
    if (BlockNumberIsValid(last)) {
        CHDB_SPECIAL(chdb_write_page(&w, last, false))->next = blk;
    } else {
        CHDB_META(chdb_write_meta(&w))->dir_head = blk;
    }
    chdb_write_finish(&w);
    return blk;
}

ChdbDirEntry*
chdb_dir_put(ChdbPages* p, const ChdbDirEntry* e, Size len) {
    char* key = pnstrdup(e->key, e->keylen);
    Room r    = { .need = MAXALIGN(len), .fit = InvalidBlockNumber, .last = InvalidBlockNumber };
    ChdbWrite w;
    Page page = NULL;

    r.old = (Find){ .key = key, .keylen = e->keylen, .blk = InvalidBlockNumber };
    walk_pages(p, room_visitor, &r);
    if (!BlockNumberIsValid(r.fit)) {
        /* Linked in a record of its own: the chain is whole whatever happens next. */
        r.fit = extend_chain(p, r.last);
    }
    chdb_write_begin(&w, p);
    if (BlockNumberIsValid(r.old.blk)) {
        page        = chdb_write_page(&w, r.old.blk, false);
        r.old.entry = copy_entry(page, r.old.off);
        PageIndexTupleDelete(page, r.old.off);
    }
    if (r.fit != r.old.blk) {
        page = chdb_write_page(&w, r.fit, false);
    }
    if (PageAddItem(page, (Item)e, len, InvalidOffsetNumber, false, false) ==
        InvalidOffsetNumber) {
        elog(ERROR, "chdb_search: could not add a blob to its directory page");
    }
    chdb_write_finish(&w);
    pfree(key);
    return r.old.entry;
}

ChdbDirEntry*
chdb_dir_remove(ChdbPages* p, const char* key) {
    Find f;
    ChdbWrite w;

    if (!find(p, key, false, &f)) {
        return NULL;
    }
    chdb_write_begin(&w, p);

    Page page       = chdb_write_page(&w, f.blk, false);
    ChdbDirEntry* e = copy_entry(page, f.off);

    PageIndexTupleDelete(page, f.off);
    chdb_write_finish(&w);
    return e;
}

/* ---- listing ---- */

typedef struct Listing {
    const char* prefix;
    size_t len;
    ChdbDirSink sink;
    void* ud;
} Listing;

static bool
list_visitor(void* ud, BlockNumber blk, Page page) {
    Listing* l       = ud;
    OffsetNumber max = PageGetMaxOffsetNumber(page);

    for (OffsetNumber off = FirstOffsetNumber; off <= max; off++) {
        ChdbDirEntry* e = entry_at(page, off);

        if (e->keylen >= l->len && memcmp(e->key, l->prefix, l->len) == 0) {
            l->sink(l->ud, e);
        }
    }
    return true;
}

void
chdb_dir_list(ChdbPages* p, const char* prefix, ChdbDirSink sink, void* ud) {
    Listing l = { .prefix = prefix, .len = strlen(prefix), .sink = sink, .ud = ud };

    walk_pages(p, list_visitor, &l);
}
