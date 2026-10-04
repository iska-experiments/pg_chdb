/*
 * Reclaiming what a crash left in a chdb index relation: pages no directory
 * entry reaches. A write takes pages before its entry is put, so a worker
 * that died in between left them off the free stack and out of every map
 * chain; a copy raises a chain's share count before its entry is put, so a
 * death there left the count high. Both are found by walking the directory
 * and the free stack, marking what they reach, and comparing. Only a
 * relation whose metapage says CHDB_META_DIRTY needs it, which the worker
 * clears when it stops with nothing pending (pages.h). See pagestore.h.
 */

#include "postgres.h"

#include "storage/bufmgr.h"
#include "utils/hsearch.h"

#include "dir.h"
#include "recover.h"

typedef struct Marks {
    ChdbPages* p;
    uint8* seen;  /* a bit per block */
    HTAB* chains; /* map head -> entries naming it */
} Marks;

typedef struct Chain {
    BlockNumber head;
    uint32 refs;
} Chain;

static void
mark(Marks* m, BlockNumber blk) {
    if (blk < m->p->nblocks) {
        m->seen[blk / 8] |= 1 << (blk % 8);
    }
}

static bool
marked(Marks* m, BlockNumber blk) {
    return (m->seen[blk / 8] >> (blk % 8)) & 1;
}

/* Marks a list page's numbers and the page itself, returning the chain's next. */
static BlockNumber
mark_list_page(Marks* m, BlockNumber blk) {
    Buffer buf       = chdb_pages_read(m->p, blk, BUFFER_LOCK_SHARE);
    Page page        = BufferGetPage(buf);
    BlockNumber next = CHDB_SPECIAL(page)->next;
    uint32 count     = CHDB_LIST(page)->count;

    mark(m, blk);
    for (uint32 i = 0; i < count; i++) {
        mark(m, CHDB_LIST_BLOCKS(page)[i]);
    }
    UnlockReleaseBuffer(buf);
    return next;
}

static void
mark_chain(Marks* m, BlockNumber head) {
    for (BlockNumber at = head; BlockNumberIsValid(at); at = mark_list_page(m, at)) {}
}

/* A directory entry: its chain is marked the first time an entry names it. */
static void
mark_entry(void* ud, const ChdbDirEntry* e) {
    Marks* m = ud;
    bool found;

    if (CHDB_DIR_ENTRY_INLINE(e)) {
        return;
    }

    Chain* c = hash_search(m->chains, &e->map, HASH_ENTER, &found);

    if (!found) {
        c->refs = 0;
        mark_chain(m, e->map);
    }
    c->refs++;
}

/* The directory pages themselves, which the entry listing does not name. */
static void
mark_directory(Marks* m) {
    for (BlockNumber at = m->p->meta.dir_head; BlockNumberIsValid(at);) {
        Buffer buf = chdb_pages_read(m->p, at, BUFFER_LOCK_SHARE);

        mark(m, at);
        at = CHDB_SPECIAL(BufferGetPage(buf))->next;
        UnlockReleaseBuffer(buf);
    }
}

/* Puts a chain's share count right where the entries say otherwise. */
static void
fix_refs(Marks* m) {
    HASH_SEQ_STATUS seq;
    Chain* c;

    hash_seq_init(&seq, m->chains);
    while ((c = hash_seq_search(&seq)) != NULL) {
        ChdbWrite w;

        chdb_write_begin(&w, m->p);

        ChdbListHeader* head = CHDB_LIST(chdb_write_page(&w, c->head, false));

        if (head->refs == c->refs) {
            chdb_write_abort(&w);
        } else {
            head->refs = c->refs;
            chdb_write_finish(&w);
        }
    }
}

void
chdb_pages_recover(ChdbPages* p) {
    HASHCTL ctl = { .keysize = sizeof(BlockNumber), .entrysize = sizeof(Chain) };
    Marks m     = {
        .p      = p,
        .seen   = palloc0((p->nblocks + 7) / 8),
        .chains = hash_create("chdb_search chains", 256, &ctl, HASH_ELEM | HASH_BLOBS),
    };
    BlockNumber* lost = palloc(p->nblocks * sizeof(BlockNumber));
    uint32 nlost      = 0;

    mark(&m, CHDB_METAPAGE_BLKNO);
    mark_chain(&m, p->meta.free_head);
    mark_directory(&m);
    chdb_dir_list(p, "", mark_entry, &m);
    for (BlockNumber blk = 1; blk < p->nblocks; blk++) {
        if (!marked(&m, blk)) {
            lost[nlost++] = blk;
        }
    }
    chdb_pages_free(p, lost, nlost);
    fix_refs(&m);
    chdb_pages_clean(p);
    if (nlost) {
        ereport(
            LOG,
            errmsg(
                "chdb_search: reclaimed %u pages of relation %u a crash left behind",
                nlost,
                p->loc.relNumber
            )
        );
    }
    hash_destroy(m.chains);
    pfree(m.seen);
    pfree(lost);
}
