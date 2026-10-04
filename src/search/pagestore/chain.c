/*
 * The pages behind a directory entry: reading a range of the blob through
 * its map chain, and the count of entries sharing the chain, which a copy
 * raises and a removal lowers, the last one giving every page back to the
 * free stack. See blob.h and pages.h.
 */

#include "postgres.h"

#include "storage/bufmgr.h"

#include "blob.h"

/* Copies the numbers of one map page out and returns its next page. */
static BlockNumber
map_page(ChdbPages* p, BlockNumber blk, BlockNumber* out, uint32* count) {
    Buffer buf       = chdb_pages_read_kind(p, blk, BUFFER_LOCK_SHARE, CHDB_PAGE_MAP);
    Page page        = BufferGetPage(buf);
    BlockNumber next = CHDB_SPECIAL(page)->next;

    *count = CHDB_LIST(page)->count;
    memcpy(out, CHDB_LIST_BLOCKS(page), *count * sizeof(BlockNumber));
    UnlockReleaseBuffer(buf);
    return next;
}

Size
chdb_blob_read(ChdbPages* p, const ChdbDirEntry* e, uint64 offset, void* buf, Size len) {
    if (offset >= e->size) {
        return 0;
    }
    len = Min(len, e->size - offset);
    if (CHDB_DIR_ENTRY_INLINE(e)) {
        memcpy(buf, CHDB_DIR_ENTRY_DATA(e) + offset, len);
        return len;
    }

    uint64 first   = offset / CHDB_DATA_PER_PAGE;
    BlockNumber at = e->map;
    BlockNumber blocks[CHDB_LIST_PER_PAGE];
    uint32 count   = 0;
    uint32 idx     = (uint32)(first % CHDB_LIST_PER_PAGE);
    Size done      = 0;

    /* To the map page covering the first page wanted. */
    for (uint64 hop = first / CHDB_LIST_PER_PAGE; hop > 0; hop--) {
        Buffer mbuf = chdb_pages_read_kind(p, at, BUFFER_LOCK_SHARE, CHDB_PAGE_MAP);

        at = CHDB_SPECIAL(BufferGetPage(mbuf))->next;
        UnlockReleaseBuffer(mbuf);
    }
    at = map_page(p, at, blocks, &count);
    while (done < len) {
        if (idx == count) {
            at  = map_page(p, at, blocks, &count);
            idx = 0;
            continue;
        }

        Buffer dbuf =
            chdb_pages_read_kind(p, blocks[idx++], BUFFER_LOCK_SHARE, CHDB_PAGE_DATA);
        Page page   = BufferGetPage(dbuf);
        Size inpage = (Size)((offset + done) % CHDB_DATA_PER_PAGE);
        Size n      = Min(CHDB_DATA_PER_PAGE - inpage, len - done);

        if (CHDB_PAGE_START + inpage + n > ((PageHeader)page)->pd_lower) {
            ereport(
                ERROR,
                errcode(ERRCODE_DATA_CORRUPTED),
                errmsg("chdb_search: blob \"%.*s\" is shorter than its directory entry",
                       e->keylen, e->key)
            );
        }
        memcpy((char*)buf + done, (char*)page + CHDB_PAGE_START + inpage, n);
        UnlockReleaseBuffer(dbuf);
        done += n;
    }
    return len;
}

/* ---- the pages behind an entry ---- */

void
chdb_blob_share(ChdbPages* p, const ChdbDirEntry* e) {
    ChdbWrite x;

    if (CHDB_DIR_ENTRY_INLINE(e)) {
        return;
    }
    chdb_write_begin(&x, p);
    CHDB_LIST(chdb_write_page(&x, e->map, false))->refs++;
    chdb_write_finish(&x);
}

void
chdb_blob_release(ChdbPages* p, const ChdbDirEntry* e) {
    ChdbWrite x;

    if (CHDB_DIR_ENTRY_INLINE(e)) {
        return;
    }
    chdb_write_begin(&x, p);

    ChdbListHeader* head = CHDB_LIST(chdb_write_page(&x, e->map, false));

    if (head->refs > 1) {
        head->refs--;
        chdb_write_finish(&x);
        return;
    }
    chdb_write_abort(&x);

    /* The last entry: every data page and the map pages themselves go. */
    uint32 cap        = 1024, n = 0;
    BlockNumber* all  = palloc(cap * sizeof(BlockNumber));
    BlockNumber blocks[CHDB_LIST_PER_PAGE];

    for (BlockNumber at = e->map; BlockNumberIsValid(at);) {
        uint32 count;
        BlockNumber next = map_page(p, at, blocks, &count);

        if (n + count + 1 > cap) {
            cap = Max(2 * cap, n + count + 1);
            all = repalloc(all, cap * sizeof(BlockNumber));
        }
        memcpy(all + n, blocks, count * sizeof(BlockNumber));
        n += count;
        all[n++] = at;
        at       = next;
    }
    chdb_pages_free(p, all, n);
    pfree(all);
}
