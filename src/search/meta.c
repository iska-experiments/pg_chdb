/*
 * The index relation's metapage: a WAL-logged page tying the relation to
 * its ClickHouse store. The generation names the store table,
 * idx_<oid>.t_<generation>, so a rebuild that rolls back leaves the table
 * the metapage still names untouched; flushed_lsn records how far the
 * store was written, for the eye. The page also carries the heads of the
 * blob store's chains (pagestore/pages.h), which the worker moves under
 * the same buffer lock a flush takes here, so neither write loses the
 * other's.
 *
 * The store lives in this relation's pages, written into the WAL ahead of
 * the commits that depend on it, so a crash, a restore or a rewind leave
 * the two as one: what remains to check is that this server can serve the
 * pages at all, which one in recovery cannot until a standby's worker
 * learns to read them.
 */

#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/xlog.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"

#include "search.h"

/* ---- the fail-safe check ---- */

bool
chdb_search_store_unavailable(Relation index) {
    return RecoveryInProgress();
}

void
chdb_search_check_available(Relation index, bool* skip) {
    *skip = false;
    if (!chdb_search_store_unavailable(index)) {
        return;
    }
    if (chdb_search_unavailable_index == CHDB_UNAVAILABLE_SKIP) {
        *skip = true;
        return;
    }
    ereport(
        ERROR,
        errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
        errmsg(
            "chdb index \"%s\" is not available on this server",
            RelationGetRelationName(index)
        ),
        errdetail("Its store is not served on a server in recovery."),
        errhint("Query the index on the primary, or once the server is promoted.")
    );
}

/* ---- the metapage ---- */

/* Writes a fresh metapage and returns the generation it chose. */
uint64
chdb_meta_init(Relation index, ForkNumber fork) {
    /* No concurrent inserters can exist yet, as in contrib/bloom. */
    Buffer buf = ReadBufferExtended(index, fork, P_NEW, RBM_NORMAL, NULL);

    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    Assert(BufferGetBlockNumber(buf) == CHDB_METAPAGE_BLKNO);

    GenericXLogState* xlog = GenericXLogStart(index);
    Page page = GenericXLogRegisterBuffer(xlog, buf, GENERIC_XLOG_FULL_IMAGE);

    PageInit(page, BLCKSZ, 0);
    ChdbMetaPageData* meta = CHDB_META(page);
    uint64 generation      = pg_prng_uint64(&pg_global_prng_state);

    meta->magic       = CHDB_META_MAGIC;
    meta->version     = CHDB_META_VERSION;
    meta->generation  = generation;
    meta->flushed_lsn = 0;
    meta->dir_head    = InvalidBlockNumber;
    meta->free_head   = InvalidBlockNumber;
    meta->flags       = 0;
    ((PageHeader)page)->pd_lower += sizeof(ChdbMetaPageData);

    GenericXLogFinish(xlog);
    UnlockReleaseBuffer(buf);
    return generation;
}

void
chdb_meta_read(Relation index, ChdbMetaPageData* out) {
    Buffer buf = ReadBuffer(index, CHDB_METAPAGE_BLKNO);

    LockBuffer(buf, BUFFER_LOCK_SHARE);
    *out = *CHDB_META(BufferGetPage(buf));
    UnlockReleaseBuffer(buf);

    if (out->magic != CHDB_META_MAGIC) {
        elog(
            ERROR, "relation \"%s\" is not a chdb index", RelationGetRelationName(index)
        );
    }
}

uint64
chdb_meta_generation(Relation index) {
    ChdbMetaPageData meta;

    chdb_meta_read(index, &meta);
    return meta.generation;
}

/*
 * Moves flushed_lsn on to the current WAL position, never back. Runs after
 * a flush, before the commit; the rows of a commit that never came, after
 * a crash in between, are in the store with the transaction id that wrote
 * them, which the scan checks (scan.c).
 */
static void
advance_metapage(Relation index) {
    uint64 lsn = GetXLogInsertRecPtr();
    Buffer buf = ReadBuffer(index, CHDB_METAPAGE_BLKNO);

    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    GenericXLogState* xlog = GenericXLogStart(index);
    Page page              = GenericXLogRegisterBuffer(xlog, buf, 0);
    ChdbMetaPageData* meta = CHDB_META(page);

    if (meta->flushed_lsn >= lsn) {
        GenericXLogAbort(xlog);
    } else {
        meta->flushed_lsn = lsn;
        GenericXLogFinish(xlog);
    }
    UnlockReleaseBuffer(buf);
}

void
chdb_meta_note_flush(Relation index) {
    advance_metapage(index);
}

/* A commit whose rows the store did not take, the server being in recovery. */
void
chdb_meta_note_skipped(Relation index) {
    advance_metapage(index);
}
