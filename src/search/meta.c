/*
 * The index relation's only page: a WAL-logged metapage tying the relation to
 * its ClickHouse store. Postgres decides what survives a crash or a restore;
 * the store is derived data, so the page records which store belongs to this
 * relation (generation) and how far it was written (flushed_lsn). The
 * generation names the store table, idx_<oid>.t_<generation>, so a rebuild
 * that rolls back leaves the table the metapage still names untouched. The
 * store keeps the same record in its meta table, one row per generation and
 * flush (protocol.h), so that the two can be compared: a store and a
 * relation that agree on the generation and the last flush are from the
 * same point in time.
 *
 * The comparison and the writes it compares run under a heavyweight lock on
 * the metapage (LockPage, as GIN takes on its own): a flush writes the store
 * and then the page, and the worker serves one request at a time, so a
 * check that read the page and then queued behind another backend's flush
 * would see the store ahead of a page that backend had yet to write, and
 * refuse a store that is current. Shared by checks, exclusive for a flush,
 * the lock keeps a check from falling between the two writes, and costs a
 * flush nothing it did not pay already at the worker.
 */

#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/xlog.h"
#include "catalog/pg_type_d.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"

#include "search.h"

static ChdbMetaPageData*
meta_of(Page page) {
    return (ChdbMetaPageData*)PageGetContents(page);
}

/* ---- the fail-safe check ---- */

/* A store proved current for this metapage state, kept in rd_amcache. */
typedef struct ChdbVerdict {
    uint64 generation;
    uint64 flushed_lsn;
} ChdbVerdict;

static bool
store_proved(Relation index, const ChdbMetaPageData* meta) {
    ChdbVerdict* v = index->rd_amcache;

    return v && v->generation == meta->generation &&
           v->flushed_lsn == meta->flushed_lsn;
}

static void
store_prove(Relation index, const ChdbMetaPageData* meta) {
    ChdbVerdict* v = index->rd_amcache;

    if (!v) {
        v                 = MemoryContextAlloc(index->rd_indexcxt, sizeof(*v));
        index->rd_amcache = v;
    }
    v->generation  = meta->generation;
    v->flushed_lsn = meta->flushed_lsn;
}

/*
 * Asks the store what it holds for the metapage's generation: whether its
 * table exists, and the last flush its meta table records. Nothing is
 * refused on the way, as the request names no generation and the meta table
 * exists in every store the worker has touched. NULL when the two agree,
 * else why not, palloc'd in the caller's context.
 */
static char*
store_mismatch(Relation index, const ChdbMetaPageData* meta) {
    Oid oid           = RelationGetRelid(index);
    MemoryContext cxt = AllocSetContextCreate(
        CurrentMemoryContext, "chdb_search store check", ALLOCSET_SMALL_SIZES
    );
    MemoryContext old = MemoryContextSwitchTo(cxt);
    const char* why   = NULL;
    char* sql         = psprintf(
        "SELECT count(), max(lsn), (SELECT count() FROM system.tables "
        "WHERE database = 'idx_%u' AND name = 't_" UINT64_FORMAT "') "
        "FROM " CHDB_STORE_META_FMT " WHERE generation = " UINT64_FORMAT,
        oid,
        meta->generation,
        oid,
        meta->generation
    );
    ChdbStream* s = chdb_search_stream_query(
        oid, 0, sql, (Oid[]){ INT8OID, INT8OID, INT8OID }, 3, cxt
    );

    if (!chdb_search_stream_next(s, NULL)) {
        why = "The store did not say what it holds.";
    } else if (DatumGetInt64(s->vals[2]) == 0 || DatumGetInt64(s->vals[0]) == 0) {
        why = psprintf(
            "The store has no table for generation " UINT64_FORMAT ".", meta->generation
        );
    } else if ((uint64)DatumGetInt64(s->vals[1]) != meta->flushed_lsn) {
        why = psprintf(
            "The store was last flushed at %X/%X, the index at %X/%X.",
            LSN_FORMAT_ARGS((XLogRecPtr)DatumGetInt64(s->vals[1])),
            LSN_FORMAT_ARGS((XLogRecPtr)meta->flushed_lsn)
        );
    }
    chdb_search_stream_close(s);
    MemoryContextSwitchTo(old);

    char* result = why ? pstrdup(why) : NULL;

    MemoryContextDelete(cxt);
    return result;
}

/*
 * NULL when the store can be served, else the reason, palloc'd. Phase 0: a
 * server in recovery has only a store copied as files, which a backup, a
 * standby or pg_rewind leaves stale, and its per-database worker does not
 * start until recovery ends. Out of recovery the store is asked, once per
 * state of the metapage: a REINDEX changes the generation and invalidates
 * the relcache, a flush by another backend raises flushed_lsn, so a cached
 * verdict is never for a store that changed since, and a flush of this
 * backend's own proves the state it leaves (chdb_meta_note_flush). The
 * hazards it guards against, a crash, a restore, a rewind, all restart the
 * server and with it every cache.
 */
static char*
unavailable_why(Relation index, bool* recovering) {
    ChdbMetaPageData meta;
    char* why = NULL;

    *recovering = RecoveryInProgress();
    if (*recovering) {
        return pstrdup("Its store is not current on a server in recovery.");
    }
    /* Held while the store is asked, so no flush is half done meanwhile. */
    LockPage(index, CHDB_METAPAGE_BLKNO, ShareLock);
    chdb_meta_read(index, &meta);
    if (!store_proved(index, &meta)) {
        why = store_mismatch(index, &meta);
        if (!why) {
            store_prove(index, &meta);
        }
    }
    UnlockPage(index, CHDB_METAPAGE_BLKNO, ShareLock);
    return why;
}

bool
chdb_search_store_unavailable(Relation index) {
    bool recovering;
    char* why = unavailable_why(index, &recovering);

    if (why) {
        pfree(why);
    }
    return why != NULL;
}

void
chdb_search_check_available(Relation index, bool* skip) {
    bool recovering;
    char* why = unavailable_why(index, &recovering);

    *skip = false;
    if (!why) {
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
        errdetail("%s", why),
        recovering ? errhint("REINDEX the index once the server is out of recovery.")
                   : errhint(
                         "REINDEX INDEX \"%s\" rebuilds its store.",
                         RelationGetRelationName(index)
                     )
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

/* Moves flushed_lsn on to `lsn`, never back, and returns what the page holds. */
static uint64
advance_metapage(Relation index, uint64 lsn) {
    Buffer buf = ReadBuffer(index, CHDB_METAPAGE_BLKNO);

    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    GenericXLogState* xlog = GenericXLogStart(index);
    Page page              = GenericXLogRegisterBuffer(xlog, buf, 0);
    ChdbMetaPageData* meta = meta_of(page);

    if (meta->flushed_lsn >= lsn) {
        GenericXLogAbort(xlog);
        lsn = meta->flushed_lsn;
    } else {
        meta->flushed_lsn = lsn;
        GenericXLogFinish(xlog);
    }
    UnlockReleaseBuffer(buf);
    return lsn;
}

/*
 * Records that the store now holds everything logged so far, in the store's
 * meta table and then in the metapage. Runs after the flush, before commit:
 * a crash in between leaves store rows for heap tuples that were never
 * committed, and a restored relation shows an older LSN than the store. The
 * heap fetch does not hide those rows: one past the heap's end errors, and
 * one whose TID a later insert reused returns that other row, so a store
 * ahead of its metapage must be detected and rebuilt, not read.
 *
 * The store is written first, so that a failure between the two leaves it
 * ahead, which is refused, rather than the metapage ahead of a store that
 * is complete. Flushes of one index by several backends take the lock in
 * turn, each with a position past the last, so the page always holds the
 * store's highest: both sides record this flush when the lock is released,
 * which proves the store for the state it leaves, as a check would.
 */
void
chdb_meta_note_flush(Relation index) {
    ChdbMetaPageData current;

    LockPage(index, CHDB_METAPAGE_BLKNO, ExclusiveLock);
    chdb_meta_read(index, &current);

    uint64 lsn = GetXLogInsertRecPtr();

    chdb_search_run(
        RelationGetRelid(index),
        current.generation,
        psprintf(
            "INSERT INTO " CHDB_STORE_META_FMT
            " (generation, lsn) VALUES (" UINT64_FORMAT ", " UINT64_FORMAT ")",
            RelationGetRelid(index),
            current.generation,
            lsn
        )
    );
    current.flushed_lsn = advance_metapage(index, lsn);
    store_prove(index, &current);
    UnlockPage(index, CHDB_METAPAGE_BLKNO, ExclusiveLock);
}

/*
 * Records a commit whose rows the store did not take, because the check
 * refused it and chdb_search.unavailable_index is skip. The metapage moves
 * on all the same, so that the store, or any copy of it put back by hand,
 * can never match the index again short of the REINDEX that rebuilds it
 * from the heap the rows went to.
 */
void
chdb_meta_note_skipped(Relation index) {
    LockPage(index, CHDB_METAPAGE_BLKNO, ExclusiveLock);
    advance_metapage(index, GetXLogInsertRecPtr());
    UnlockPage(index, CHDB_METAPAGE_BLKNO, ExclusiveLock);
}
