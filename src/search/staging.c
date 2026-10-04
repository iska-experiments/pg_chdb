/*
 * Sending a transaction's buffered rows to the worker: as Native blocks into
 * the index's table at commit, or, past chdb_search.flush_threshold, into a
 * staging table <table>_tx_<fxid> along the way, which commit copies into
 * the table and drops. The buffer itself is buffer.c's.
 *
 * The staging table is named by the full transaction id. Its only cleanup
 * from this backend is the drop registered for abort, which a crash or OOM
 * kill skips; the VACUUM sweep in vacuum.c then removes it once the
 * transaction is over, and a 32-bit xid come round after wraparound would
 * have found it in the way.
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "utils/memutils.h"

#include "buffer.h"

/* Sends the buffered rows into `table` as Native blocks. */
static void
send_rows(Pending* p, const char* table) {
    if (chdb_rowwriter_rows(p->rw) == 0) {
        return;
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
        chdb_search_insert(conn, p->indexoid, CHDB_SEARCH_NO_GENERATION, sql);
        chdb_channel_write(chdb_search_channel(conn), block, len);
        chdb_search_finish(conn);
    }
    PG_FINALLY();
    { chdb_search_close(conn); }
    PG_END_TRY();
    pfree(block);
}

void
chdb_search_stage_rows(Pending* p) {
    if (!p->staging) {
        MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

        p->staging = psprintf(
            "%s_tx_" UINT64_FORMAT,
            p->table,
            U64FromFullTransactionId(GetTopFullTransactionId())
        );
        MemoryContextSwitchTo(old);

        char* drop = psprintf("DROP TABLE IF EXISTS %s", p->staging);

        /* Registered first, so a CREATE that fails halfway is undone too. */
        chdb_search_drop_statement_on_abort(p->indexoid, drop);
        chdb_search_run(p->indexoid, drop);
        chdb_search_run(
            p->indexoid, psprintf("CREATE TABLE %s AS %s", p->staging, p->table)
        );
    }
    send_rows(p, p->staging);
}

void
chdb_search_drop_staging(Pending* p) {
    chdb_search_run(p->indexoid, psprintf("DROP TABLE IF EXISTS %s", p->staging));
    chdb_search_forget_statement(p->indexoid, p->staging);
    p->staging = NULL;
}

void
chdb_search_flush_pending(Pending* p) {
    if (p->staging) {
        send_rows(p, p->staging);
        chdb_search_run(
            p->indexoid,
            psprintf("INSERT INTO %s SELECT * FROM %s", p->table, p->staging)
        );
        chdb_search_run(p->indexoid, psprintf("DROP TABLE %s", p->staging));
        chdb_search_forget_statement(p->indexoid, p->staging);
    } else {
        send_rows(p, p->table);
    }
}
