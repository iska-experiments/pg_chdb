/*
 * The index relation's only page: a WAL-logged metapage tying the relation to
 * its ClickHouse store. Postgres decides what survives a crash or a restore;
 * the store is derived data, so the page records which store belongs to this
 * relation (generation) and how far it was written (flushed_lsn). The
 * generation names the store table, idx_<oid>.t_<generation>, so a rebuild
 * that rolls back leaves the table the metapage still names untouched, and
 * the worker compares the LSN on open to have a stale index rebuilt.
 */

#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/xlog.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"

#include "search.h"

bool
chdb_search_store_unavailable(Relation index pg_attribute_unused()) {
    /*
     * Phase 0: a server in recovery has only a store copied as files, which a
     * backup, a standby or pg_rewind leaves stale, and its per-database worker
     * does not start until recovery ends. The generation and LSN checks
     * against the worker (follow-up) will catch a store that is stale without
     * the server being in recovery.
     */
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
        errdetail("Its store is not current on a server in recovery."),
        errhint("REINDEX the index once the server is out of recovery.")
    );
}

static ChdbMetaPageData*
meta_of(Page page) {
    return (ChdbMetaPageData*)PageGetContents(page);
}

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
    ChdbMetaPageData* meta = meta_of(page);
    uint64 generation      = pg_prng_uint64(&pg_global_prng_state);

    meta->magic       = CHDB_META_MAGIC;
    meta->version     = CHDB_META_VERSION;
    meta->generation  = generation;
    meta->flushed_lsn = 0;
    ((PageHeader)page)->pd_lower += sizeof(ChdbMetaPageData);

    GenericXLogFinish(xlog);
    UnlockReleaseBuffer(buf);
    return generation;
}

void
chdb_meta_read(Relation index, ChdbMetaPageData* out) {
    Buffer buf = ReadBuffer(index, CHDB_METAPAGE_BLKNO);

    LockBuffer(buf, BUFFER_LOCK_SHARE);
    *out = *meta_of(BufferGetPage(buf));
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
 * Records that the store now holds everything logged so far. Runs after the
 * flush, before commit: a crash in between leaves store rows for heap tuples
 * that were never committed, and a restored relation shows an older LSN than
 * the store. The heap fetch does not hide those rows: one past the heap's end
 * errors, and one whose TID a later insert reused returns that other row, so
 * a store ahead of its metapage must be detected and rebuilt, not read.
 */
void
chdb_meta_note_flush(Relation index) {
    Buffer buf = ReadBuffer(index, CHDB_METAPAGE_BLKNO);

    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    GenericXLogState* xlog = GenericXLogStart(index);
    Page page              = GenericXLogRegisterBuffer(xlog, buf, 0);
    ChdbMetaPageData* meta = meta_of(page);

    if (meta->magic != CHDB_META_MAGIC) {
        GenericXLogAbort(xlog);
        UnlockReleaseBuffer(buf);
        elog(
            ERROR, "relation \"%s\" is not a chdb index", RelationGetRelationName(index)
        );
    }
    meta->flushed_lsn = GetXLogInsertRecPtr();
    GenericXLogFinish(xlog);
    UnlockReleaseBuffer(buf);
}
