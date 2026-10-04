/*
 * Sending a transaction's buffered rows to the worker: as Native blocks into
 * the index's table at commit, or, past chdb_search.flush_threshold, into a
 * staging table <table>_tx_<fxid> along the way, whose parts commit attaches
 * to the table before dropping it. The buffer itself is buffer.c's, and marks.c keeps
 * the savepoint levels whose rows were staged, so that a rollback of one
 * excludes its rows (by their xmin) from what the staging table contributes.
 *
 * The staging table is named by the full transaction id, and defined as the
 * index's table is but for its name, UUID and key prefix (ddl.c), on the
 * index's own storage, so that a worker starting afresh can attach it again
 * from what the index's pages hold (attach.c). Its only cleanup from this
 * backend is the drop buffer.c runs at abort, which a crash or OOM kill
 * skips; the VACUUM sweep in vacuum.c then removes it once the transaction
 * is over, and a 32-bit xid come round after wraparound would have found it
 * in the way. The drops name no generation: they clean up after a build
 * whose table may be gone already.
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "utils/memutils.h"

#include "buffer.h"
#include "query.h"

/*
 * Sends the buffered rows into `table` as Native blocks; false when there
 * were none. Taking the block empties the buffer, so a send that then fails
 * has lost rows the store may or may not hold, and the buffer is poisoned:
 * the transaction can only roll back. (At the top level the error aborts it
 * anyway; inside a savepoint it could be caught.)
 */
static bool
send_rows(Pending* p, const char* table) {
    if (chdb_rowwriter_rows(p->rw) == 0) {
        return false;
    }

    size_t len;
    char* sql = psprintf("INSERT INTO %s %s", table, p->collist);

    /* Logged with the rows, so tests see what a savepoint left in the buffer. */
    chdb_search_log_sql(
        "insert", psprintf("%s -- %zu rows", sql, chdb_rowwriter_rows(p->rw))
    );

    void* block          = chdb_rowwriter_take(p->rw, &len);
    chdbSearchConn* conn = chdb_search_connect();

    PG_TRY();
    {
        chdb_search_insert(conn, p->indexoid, p->generation, sql);
        chdb_channel_write(chdb_search_channel(conn), block, len);
        chdb_search_finish(conn);
    }
    PG_CATCH();
    {
        p->poisoned = true;
        chdb_search_close(conn);
        PG_RE_THROW();
    }
    PG_END_TRY();
    chdb_search_close(conn);
    pfree(block);
    return true;
}

void
chdb_search_stage_rows(Pending* p, Relation index) {
    if (!p->staging) {
        uint64 fxid       = U64FromFullTransactionId(GetTopFullTransactionId());
        MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

        p->staging = chdb_search_staging_of(p->indexoid, p->generation, fxid);
        MemoryContextSwitchTo(old);

        /* Named before it is made, so a CREATE that fails halfway is dropped too. */
        chdb_search_run(
            p->indexoid, 0, psprintf("DROP TABLE IF EXISTS %s SYNC", p->staging)
        );
        chdb_search_run(
            p->indexoid, p->generation, chdb_search_staging_sql(index, fxid)
        );
    }
    if (send_rows(p, p->staging)) {
        chdb_search_marks_shipped(p);
    }
}

/*
 * For a scan of the index: the transaction's staging table, or NULL when it
 * has none. The rows the transaction has buffered for the index go there
 * first, so that the scan finds every row it inserted; *excluded is the
 * `xmin NOT IN` list the table is to be read with, NULL for none.
 */
char*
chdb_search_staged_table(Relation index, const char** excluded) {
    Pending* p = chdb_search_pending_of(RelationGetRelid(index));

    *excluded = NULL;
    if (!p) {
        return NULL;
    }
    if (chdb_rowwriter_rows(p->rw)) {
        chdb_search_stage_rows(p, index);
    }
    if (p->staging) {
        *excluded = chdb_search_excluded_xids(p);
    }
    return p->staging;
}

/* Drops the staging table with `run`. */
static void
drop_staging(Pending* p, void (*run)(Oid, uint64, const char*)) {
    run(p->indexoid, 0, psprintf("DROP TABLE IF EXISTS %s SYNC", p->staging));
    p->staging = NULL;
}

void
chdb_search_drop_staging(Pending* p) {
    drop_staging(p, chdb_search_run);
}

/* What a failed drop leaves, the sweep in vacuum.c removes once it is stale. */
void
chdb_search_abandon_staging(Pending* p) {
    drop_staging(p, chdb_search_try_run);
}

/*
 * What the buffer still holds goes straight into the table, and then the
 * staged rows follow it. The staging table was created AS the table, so its
 * parts attach as they are: ClickHouse links them into the table's
 * directory, whatever their size, where an INSERT ... SELECT would read and
 * write every row again (1.4 ms against 142 ms for 300k rows, measured
 * through the worker). The copy is kept for the staging table a rolled-back
 * savepoint left rows in, which attaching could not leave out.
 */
void
chdb_search_flush_pending(Pending* p) {
    send_rows(p, p->table);
    if (!p->staging) {
        return;
    }

    char* excluded = chdb_search_excluded_xids(p);

    chdb_search_run(
        p->indexoid,
        p->generation,
        excluded ? psprintf(
                       "INSERT INTO %s SELECT * FROM %s WHERE xmin NOT IN (%s)",
                       p->table,
                       p->staging,
                       excluded
                   )
                 : psprintf(
                       "ALTER TABLE %s ATTACH PARTITION tuple() FROM %s",
                       p->table,
                       p->staging
                   )
    );
    chdb_search_run(
        p->indexoid, p->generation, psprintf("DROP TABLE %s SYNC", p->staging)
    );
    p->staging = NULL;
}
