/*
 * Page I/O for the blobs of a chdb index relation: opening a relation by
 * its locator, reading its pages, and the generic WAL record every write is
 * made of, with pages taken from the free stack or by extending the
 * relation and given back to the stack. See pages.h for the layout.
 *
 * Nothing here goes through the relcache: the worker writes into relations
 * whose CREATE INDEX has not committed, which it could not open by OID, so
 * buffers are read by locator and WAL-logged through a stand-in relation
 * that says only what GenericXLogStart asks, that the relation is permanent.
 * A backend doing the same on an index it has open passes its own entry.
 */

#include "postgres.h"

#include "catalog/pg_class.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"

#include "pages.h"

static FormData_pg_class standin_class = { .relpersistence = RELPERSISTENCE_PERMANENT };
static RelationData standin            = { .rd_rel = &standin_class };

bool
chdb_pages_open(ChdbPages* p, RelFileLocator loc, Relation rel) {
    SMgrRelation smgr = smgropen(loc, INVALID_PROC_NUMBER);

    p->loc = loc;
    p->rel = rel ? rel : &standin;
    if (!smgrexists(smgr, MAIN_FORKNUM)) {
        return false; /* dropped, or its drop is a checkpoint away */
    }
    p->nblocks = smgrnblocks(smgr, MAIN_FORKNUM);
    if (p->nblocks == 0) {
        return false; /* truncated by its drop, or not yet initialized */
    }

    Buffer buf = chdb_pages_read(p, CHDB_METAPAGE_BLKNO, BUFFER_LOCK_SHARE);

    p->meta = *CHDB_META(BufferGetPage(buf));
    UnlockReleaseBuffer(buf);
    if (p->meta.magic != CHDB_META_MAGIC || p->meta.version != CHDB_META_VERSION) {
        ereport(
            ERROR,
            errcode(ERRCODE_DATA_CORRUPTED),
            errmsg(
                "chdb_search: relation %u is not a chdb index of this version",
                loc.relNumber
            ),
            errhint("REINDEX the index.")
        );
    }
    return true;
}

static void
corrupt(ChdbPages* p, const char* what) {
    ereport(
        ERROR,
        errcode(ERRCODE_DATA_CORRUPTED),
        errmsg("chdb_search: %s in relation %u", what, p->loc.relNumber),
        errhint("REINDEX the index.")
    );
}

Buffer
chdb_pages_read(ChdbPages* p, BlockNumber blk, int mode) {
    if (!BlockNumberIsValid(blk)) {
        corrupt(p, "a page chain is broken");
    }
    if (blk >= p->nblocks) {
        /* A backend listing while the worker extends the relation sees new pages. */
        p->nblocks = smgrnblocks(smgropen(p->loc, INVALID_PROC_NUMBER), MAIN_FORKNUM);
        if (blk >= p->nblocks) {
            corrupt(p, "a page chain names a page past the end");
        }
    }

    Buffer buf =
        ReadBufferWithoutRelcache(p->loc, MAIN_FORKNUM, blk, RBM_NORMAL, NULL, true);

    LockBuffer(buf, mode);
    return buf;
}

Buffer
chdb_pages_read_kind(ChdbPages* p, BlockNumber blk, int mode, ChdbPageKind kind) {
    Buffer buf = chdb_pages_read(p, blk, mode);

    if (blk == CHDB_METAPAGE_BLKNO || CHDB_SPECIAL(BufferGetPage(buf))->kind != kind) {
        UnlockReleaseBuffer(buf);
        corrupt(p, "a page is not of the kind its chain says");
    }
    return buf;
}

/* ---- one record ---- */

void
chdb_write_begin(ChdbWrite* w, ChdbPages* p) {
    memset(w, 0, sizeof(*w));
    w->p    = p;
    w->xlog = GenericXLogStart(p->rel);
}

static Page
register_buffer(ChdbWrite* w, Buffer buf, int flags) {
    if (w->n == MAX_GENERIC_XLOG_PAGES) {
        elog(ERROR, "chdb_search: too many pages in one WAL record");
    }
    w->bufs[w->n]  = buf;
    w->pages[w->n] = GenericXLogRegisterBuffer(w->xlog, buf, flags);
    return w->pages[w->n++];
}

Page
chdb_write_meta(ChdbWrite* w) {
    if (!w->meta) {
        w->meta = register_buffer(
            w, chdb_pages_read(w->p, CHDB_METAPAGE_BLKNO, BUFFER_LOCK_EXCLUSIVE), 0
        );
    }
    return w->meta;
}

Page
chdb_write_page(ChdbWrite* w, BlockNumber blk, bool full) {
    if (blk == CHDB_METAPAGE_BLKNO) {
        return chdb_write_meta(w);
    }
    return register_buffer(
        w,
        chdb_pages_read(w->p, blk, BUFFER_LOCK_EXCLUSIVE),
        full ? GENERIC_XLOG_FULL_IMAGE : 0
    );
}

/* Pops a page off the free stack, or returns InvalidBuffer when it is empty. */
static Buffer
pop_free(ChdbWrite* w, BlockNumber* blk) {
    /* The record's own image of the metapage, when it has one, is ahead. */
    ChdbMetaPageData* meta = w->meta ? CHDB_META(w->meta) : &w->p->meta;

    if (!BlockNumberIsValid(meta->free_head)) {
        return InvalidBuffer;
    }

    Buffer top = chdb_pages_read_kind(
        w->p, meta->free_head, BUFFER_LOCK_EXCLUSIVE, CHDB_PAGE_FREE
    );

    if (CHDB_LIST(BufferGetPage(top))->count > 0) {
        /* A number off the top page. */
        Page page         = register_buffer(w, top, 0);
        ChdbListHeader* h = CHDB_LIST(page);

        *blk = CHDB_LIST_BLOCKS(page)[--h->count];
        ((PageHeader)page)->pd_lower -= sizeof(BlockNumber);
        if (*blk == CHDB_METAPAGE_BLKNO || *blk == meta->free_head) {
            corrupt(w->p, "the free stack names a page in use");
        }
        return chdb_pages_read(w->p, *blk, BUFFER_LOCK_EXCLUSIVE);
    }
    /* The top page itself, the one under it becoming the top. */
    *blk                                     = meta->free_head;
    CHDB_META(chdb_write_meta(w))->free_head = CHDB_SPECIAL(BufferGetPage(top))->next;
    return top;
}

Page
chdb_write_alloc(ChdbWrite* w, ChdbPageKind kind, BlockNumber* blk) {
    Buffer buf = pop_free(w, blk);

    if (buf == InvalidBuffer) {
        SMgrRelation smgr = smgropen(w->p->loc, INVALID_PROC_NUMBER);
        uint32 extended;

        *blk = ExtendBufferedRelBy(
            BMR_SMGR(smgr, RELPERSISTENCE_PERMANENT),
            MAIN_FORKNUM,
            NULL,
            EB_SKIP_EXTENSION_LOCK | EB_LOCK_FIRST,
            1,
            &buf,
            &extended
        );
        w->p->nblocks = *blk + 1;
    }
    if (!(w->p->meta.flags & CHDB_META_DIRTY)) {
        CHDB_META(chdb_write_meta(w))->flags |= CHDB_META_DIRTY;
    }

    Page page = register_buffer(w, buf, GENERIC_XLOG_FULL_IMAGE);

    PageInit(page, BLCKSZ, sizeof(ChdbPageSpecial));
    CHDB_SPECIAL(page)->kind = kind;
    CHDB_SPECIAL(page)->next = InvalidBlockNumber;
    return page;
}

static void
release_all(ChdbWrite* w) {
    for (int i = 0; i < w->n; i++) {
        UnlockReleaseBuffer(w->bufs[i]);
    }
    w->n = 0;
}

void
chdb_write_finish(ChdbWrite* w) {
    if (w->meta) {
        w->p->meta = *CHDB_META(w->meta);
    }
    GenericXLogFinish(w->xlog);
    release_all(w);
}

void
chdb_write_abort(ChdbWrite* w) {
    GenericXLogAbort(w->xlog);
    release_all(w);
}

/* ---- the free stack ---- */

/* Whether the top page of the free stack has room for another number. */
static bool
top_has_room(ChdbPages* p) {
    if (!BlockNumberIsValid(p->meta.free_head)) {
        return false;
    }

    Buffer top =
        chdb_pages_read_kind(p, p->meta.free_head, BUFFER_LOCK_SHARE, CHDB_PAGE_FREE);
    bool room = CHDB_LIST(BufferGetPage(top))->count < CHDB_LIST_PER_PAGE;

    UnlockReleaseBuffer(top);
    return room;
}

void
chdb_pages_free(ChdbPages* p, const BlockNumber* blks, uint32 n) {
    while (n > 0) {
        ChdbWrite w;
        Page top;

        chdb_write_begin(&w, p);
        if (top_has_room(p)) {
            top = chdb_write_page(&w, p->meta.free_head, false);
        } else {
            /* The first page freed becomes the top, on top of the old one. */
            top = chdb_write_page(&w, blks[0], true);
            PageInit(top, BLCKSZ, sizeof(ChdbPageSpecial));
            CHDB_SPECIAL(top)->kind = CHDB_PAGE_FREE;
            CHDB_SPECIAL(top)->next = p->meta.free_head;
            ((PageHeader)top)->pd_lower += sizeof(ChdbListHeader);
            CHDB_META(chdb_write_meta(&w))->free_head = blks[0];
            blks++;
            n--;
        }

        ChdbListHeader* h = CHDB_LIST(top);
        uint32 take       = Min(CHDB_LIST_PER_PAGE - h->count, n);

        memcpy(CHDB_LIST_BLOCKS(top) + h->count, blks, take * sizeof(BlockNumber));
        h->count += take;
        ((PageHeader)top)->pd_lower += take * sizeof(BlockNumber);
        blks += take;
        n -= take;
        chdb_write_finish(&w);
    }
}

void
chdb_pages_clean(ChdbPages* p) {
    ChdbWrite w;

    if (!(p->meta.flags & CHDB_META_DIRTY)) {
        return;
    }
    chdb_write_begin(&w, p);
    CHDB_META(chdb_write_meta(&w))->flags &= ~CHDB_META_DIRTY;
    chdb_write_finish(&w);
}
