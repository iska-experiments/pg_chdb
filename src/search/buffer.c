/*
 * The per-transaction insert buffer behind aminsert.
 *
 * aminsert never talks to the worker. It appends (ctid, xmin, values) to a
 * per-backend, per-index buffer in TopTransactionContext. At
 * XACT_EVENT_PRE_COMMIT each buffer goes to the worker as Native blocks and
 * the call waits for the acknowledgement, so a committed row is searchable as
 * soon as COMMIT returns. Abort drops the buffers.
 *
 * Subtransactions. A rolled-back savepoint must take its rows with it. Each
 * buffer keeps a stack of writer checkpoints, one per subtransaction level
 * that has inserted, taken before the level's first row. ROLLBACK TO rewinds
 * the writer to the checkpoint. Releasing a savepoint merges its level into
 * the parent's.
 *
 * Large transactions. Past chdb_search.flush_threshold a top-level
 * transaction flushes its buffer into a staging table <table>_tx_<xid>
 * instead, which pre-commit copies into the table and drops (abort drops it
 * too).
 * Inside a savepoint nothing is flushed early, because rows already sent
 * could not be taken back.
 */

#include "postgres.h"

#include "access/relation.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "pg-clickhouse-encode.h"

#include "search.h"

/* ---- per-transaction buffers ---- */

typedef struct Mark {
    SubTransactionId subid;
    pgch_checkpoint ckpt;
} Mark;

typedef struct Pending {
    Oid indexoid;
    ChdbRowWriter* rw;
    char* table;   /* idx_<oid>.t_<generation> */
    char* collist; /* (ctid, xmin, ...) for the INSERT */
    char* staging; /* <table>_tx_<xid>, set once rows have been staged */
    List* marks;   /* of Mark*, innermost last */
} Pending;

static List* pending = NIL; /* of Pending*, in TopTransactionContext */

static Pending*
find_pending(Relation index) {
    ListCell* lc;

    foreach (lc, pending) {
        Pending* p = lfirst(lc);

        if (p->indexoid == RelationGetRelid(index)) {
            return p;
        }
    }

    MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
    Pending* p        = palloc0(sizeof(*p));

    p->indexoid = RelationGetRelid(index);
    p->table    = chdb_search_table_name(index);
    p->collist  = chdb_search_column_list(index);
    p->rw       = chdb_rowwriter_new(index);
    pending     = lappend(pending, p);
    MemoryContextSwitchTo(old);
    return p;
}

static Mark*
top_mark(Pending* p) {
    return p->marks ? llast(p->marks) : NULL;
}

static void
pop_mark(Pending* p) {
    Mark* m = llast(p->marks);

    pgch_checkpoint_free(&m->ckpt);
    p->marks = list_delete_last(p->marks);
    pfree(m);
}

/* Sends the buffered rows into `table` as Native blocks. */
static void
send_rows(Pending* p, const char* table) {
    if (chdb_rowwriter_rows(p->rw) == 0) {
        return;
    }

    size_t len;
    void* block          = chdb_rowwriter_take(p->rw, &len);
    char* sql            = psprintf("INSERT INTO %s %s", table, p->collist);
    chdbSearchConn* conn = chdb_search_connect();

    PG_TRY();
    {
        chdb_search_log_sql("insert", sql);
        chdb_search_insert(conn, p->indexoid, CHDB_SEARCH_NO_GENERATION, sql);
        chdb_channel_write(chdb_search_channel(conn), block, len);
        chdb_search_finish(conn);
    }
    PG_FINALLY();
    { chdb_search_close(conn); }
    PG_END_TRY();
    pfree(block);
}

/* Moves a top-level transaction's rows into its staging table. */
static void
stage_rows(Pending* p) {
    if (!p->staging) {
        MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

        p->staging = psprintf("%s_tx_%u", p->table, GetTopTransactionId());
        MemoryContextSwitchTo(old);
        chdb_search_run(
            p->indexoid, psprintf("CREATE TABLE %s AS %s", p->staging, p->table)
        );
        chdb_search_drop_statement_on_abort(
            p->indexoid, psprintf("DROP TABLE IF EXISTS %s", p->staging)
        );
    }
    send_rows(p, p->staging);
}

bool
chdb_search_aminsert(
    Relation index,
    Datum* values,
    bool* isnull,
    ItemPointer ht_ctid,
    Relation heap,
    IndexUniqueCheck checkUnique,
    bool indexUnchanged,
    struct IndexInfo* indexInfo
) {
    Pending* p             = find_pending(index);
    SubTransactionId subid = GetCurrentSubTransactionId();
    bool nested            = GetCurrentTransactionNestLevel() > 1;
    MemoryContext old      = MemoryContextSwitchTo(TopTransactionContext);

    if (nested) {
        Mark* top = top_mark(p);

        if (!top || top->subid != subid) {
            Mark* m = palloc0(sizeof(*m));

            m->subid = subid;
            chdb_rowwriter_checkpoint(p->rw, &m->ckpt);
            p->marks = lappend(p->marks, m);
        }
    }
    MemoryContextSwitchTo(old);

    chdb_rowwriter_append(p->rw, ht_ctid, GetCurrentTransactionId(), values, isnull);

    if (!nested &&
        chdb_rowwriter_bytes(p->rw) >= (size_t)chdb_search_flush_threshold_kb * 1024) {
        stage_rows(p);
    }

    /* The index never reports a uniqueness violation. */
    return false;
}

static void
flush_pending(Pending* p) {
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

static void
reset_pending(void) {
    /* The list and buffers live in TopTransactionContext, which is going away. */
    pending = NIL;
}

static void
xact_callback(XactEvent event, void* arg) {
    ListCell* lc;

    switch (event) {
    case XACT_EVENT_PRE_PREPARE:
        foreach (lc, pending) {
            Pending* p = lfirst(lc);

            if (chdb_rowwriter_rows(p->rw) || p->staging) {
                ereport(
                    ERROR,
                    errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("cannot PREPARE a transaction that changed a chdb index")
                );
            }
        }
        break;
    case XACT_EVENT_PRE_COMMIT:
        foreach (lc, pending) {
            Pending* p = lfirst(lc);

            if (chdb_rowwriter_rows(p->rw) == 0 && !p->staging) {
                continue;
            }
            flush_pending(p);

            /* The index may have been dropped later in this transaction. */
            Relation index = try_relation_open(p->indexoid, NoLock);

            if (index) {
                chdb_meta_note_flush(index);
                relation_close(index, NoLock);
            }
        }
        break;
    case XACT_EVENT_COMMIT:
    case XACT_EVENT_ABORT:
    case XACT_EVENT_PREPARE:
        reset_pending();
        break;
    default:
        break;
    }
}

static void
subxact_callback(
    SubXactEvent event,
    SubTransactionId mySubid,
    SubTransactionId parentSubid,
    void* arg
) {
    ListCell* lc;

    if (event != SUBXACT_EVENT_COMMIT_SUB && event != SUBXACT_EVENT_ABORT_SUB) {
        return;
    }

    foreach (lc, pending) {
        Pending* p = lfirst(lc);
        Mark* top  = top_mark(p);

        if (!top || top->subid != mySubid) {
            continue;
        }
        if (event == SUBXACT_EVENT_ABORT_SUB) {
            chdb_rowwriter_rollback(p->rw, &top->ckpt);
            pop_mark(p);
        } else if (parentSubid == TopSubTransactionId) {
            pop_mark(p); /* top level aborts as a whole, no mark needed */
        } else if (
            list_length(p->marks) > 1 &&
            ((Mark*)list_nth(p->marks, list_length(p->marks) - 2))->subid == parentSubid
        ) {
            pop_mark(p); /* the parent's earlier checkpoint already covers these rows */
        } else {
            top->subid = parentSubid;
        }
    }
}

void
chdb_search_init_insert(void) {
    RegisterXactCallback(xact_callback, NULL);
    RegisterSubXactCallback(subxact_callback, NULL);
}
