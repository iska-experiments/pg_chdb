/*
 * Writing rows to the ClickHouse table: the Native row writer shared by
 * every path, the per-transaction insert buffer, and ambuild.
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
 * transaction flushes its buffer into a staging table idx_<oid>.t_tx_<xid>
 * instead, which pre-commit copies into `t` and drops (abort drops it too).
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

/* Matches src/native.c: ClickHouse coalesces small blocks itself. */
#define BLOCK_BYTES (8 * 1024 * 1024)

/* ---- TID packing ---- */

uint64
chdb_search_tid_to_u64(ItemPointer tid) {
    return ((uint64)ItemPointerGetBlockNumber(tid) << 16) |
           ItemPointerGetOffsetNumber(tid);
}

void
chdb_search_u64_to_tid(uint64 v, ItemPointer tid) {
    ItemPointerSet(tid, (BlockNumber)(v >> 16), (OffsetNumber)(v & 0xffff));
}

/* ---- row writer ---- */

struct ChdbRowWriter {
    pgch_writer* w;
    MemoryContext cxt;
    MemoryContext rowcxt;
    int natts;
    ChdbColumn* cols;
};

/* Same parsing as writer_for in src/native.c: the structure names the columns. */
ChdbRowWriter*
chdb_rowwriter_new(Relation index) {
    ChdbRowWriter* rw = palloc0(sizeof(*rw));
    char* tuple       = psprintf("Tuple(%s)", chdb_search_structure(index));
    chc_type* type;
    chc_err err = {};

    rw->cxt = CurrentMemoryContext;
    rw->rowcxt =
        AllocSetContextCreate(rw->cxt, "chdb_search row", ALLOCSET_DEFAULT_SIZES);
    rw->natts = index->rd_att->natts;
    rw->cols  = chdb_search_columns(index);

    if (chc_type_parse(tuple, strlen(tuple), &pgch_alloc, &type, &err) != CHC_OK) {
        pgch_raise(&err, ERRCODE_INVALID_PARAMETER_VALUE, "structure: ", NULL);
    }

    size_t ncols   = chc_type_n_children(type);
    pgch_col* cols = palloc0(ncols * sizeof(pgch_col));

    for (size_t i = 0; i < ncols; i++) {
        cols[i].name = chc_type_tuple_field_name(type, i, &cols[i].name_len);
        cols[i].type = chc_type_child(type, i);
    }
    rw->w = pgch_writer_new(rw->cxt, cols, ncols);

    /* Nullable arrays are ordinary in Postgres, ClickHouse has no NULL array. */
    pgch_writer_set_null_array(rw->w, PGCH_NULL_ARRAY_EMPTY);
    return rw;
}

void
chdb_rowwriter_append(
    ChdbRowWriter* rw,
    ItemPointer tid,
    TransactionId xmin,
    Datum* values,
    bool* isnull
) {
    MemoryContext old = MemoryContextSwitchTo(rw->rowcxt);

    pgch_append_datum(
        rw->w, 0, Int64GetDatum((int64)chdb_search_tid_to_u64(tid)), INT8OID, false
    );
    pgch_append_datum(rw->w, 1, Int32GetDatum((int32)xmin), INT4OID, false);
    for (int i = 0; i < rw->natts; i++) {
        Oid typ = rw->cols[i].typid;
        Datum v = values[i];
        bool n  = isnull[i];

        if (rw->cols[i].kind == CHDB_COL_TEXT) {
            /* The text index cannot hold NULL, so it is the empty string. */
            if (n) {
                v = PointerGetDatum(cstring_to_text(""));
                n = false;
            }
            typ = TEXTOID;
        } else if (rw->cols[i].kind == CHDB_COL_TEXT_ARRAY) {
            typ = ANYARRAYOID;
            if (!n) {
                Oid elem = get_element_type(rw->cols[i].typid);
                int16 len;
                bool byval;
                char align;

                get_typlenbyvalalign(elem, &len, &byval, &align);
                v = pgch_array_from_pg(v, elem, len, byval, align);
            }
        }
        pgch_append_datum(rw->w, i + 2, v, typ, n);
    }
    MemoryContextSwitchTo(old);
    MemoryContextReset(rw->rowcxt);
}

void
chdb_rowwriter_checkpoint(ChdbRowWriter* rw, pgch_checkpoint* ckpt) {
    pgch_writer_checkpoint(rw->w, ckpt);
}

void
chdb_rowwriter_rollback(ChdbRowWriter* rw, const pgch_checkpoint* ckpt) {
    pgch_writer_rollback(rw->w, ckpt);
}

size_t
chdb_rowwriter_bytes(ChdbRowWriter* rw) {
    return pgch_writer_bytes(rw->w);
}

size_t
chdb_rowwriter_rows(ChdbRowWriter* rw) {
    return pgch_writer_rows(rw->w);
}

void*
chdb_rowwriter_take(ChdbRowWriter* rw, size_t* len) {
    pgch_buf out = {};

    pgch_writer_flush(rw->w, &out, NULL);
    *len = out.len;
    return out.data;
}

void
chdb_rowwriter_free(ChdbRowWriter* rw) {
    pgch_writer_free(rw->w);
    MemoryContextDelete(rw->rowcxt);
    pfree(rw);
}

/* ---- per-transaction buffers ---- */

typedef struct Mark {
    SubTransactionId subid;
    pgch_checkpoint ckpt;
} Mark;

typedef struct Pending {
    Oid indexoid;
    ChdbRowWriter* rw;
    char* table;   /* idx_<oid>.t */
    char* collist; /* (ctid, xmin, ...) for the INSERT */
    char* staging; /* idx_<oid>.t_tx_<xid>, set once rows have been staged */
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
    p->table    = chdb_search_table_name(p->indexoid);
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
    void* block = chdb_rowwriter_take(p->rw, &len);
    char* sql   = psprintf("INSERT INTO %s %s FORMAT Native", table, p->collist);
    chdbSearchConn* conn = chdb_search_connect();

    PG_TRY();
    {
        chdb_search_log_sql("insert", sql);
        chdb_search_insert(conn, p->indexoid, sql);
        chdb_search_send(conn, block, len);
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

/* ---- ambuild ---- */

typedef struct BuildState {
    ChdbRowWriter* rw;
    chdbSearchConn* conn;
    double indtuples;
} BuildState;

static void
build_callback(
    Relation index,
    ItemPointer tid,
    Datum* values,
    bool* isnull,
    bool tupleIsAlive,
    void* state
) {
    BuildState* bs = state;

    /* Recently dead tuples stay: an older snapshot may still see them. */
    chdb_rowwriter_append(bs->rw, tid, InvalidTransactionId, values, isnull);
    bs->indtuples++;

    if (chdb_rowwriter_bytes(bs->rw) >= BLOCK_BYTES) {
        size_t len;
        void* block = chdb_rowwriter_take(bs->rw, &len);

        chdb_search_send(bs->conn, block, len);
        pfree(block);
    }
    CHECK_FOR_INTERRUPTS();
}

IndexBuildResult*
chdb_search_ambuild(Relation heap, Relation index, struct IndexInfo* indexInfo) {
    BuildState bs = {};
    double reltuples;

    if (RelationGetNumberOfBlocks(index) != 0) {
        elog(
            ERROR, "index \"%s\" already contains data", RelationGetRelationName(index)
        );
    }

    chdb_meta_init(index, MAIN_FORKNUM);
    chdb_search_create_store(index);
    /* A build that rolls back leaves a store nothing refers to. */
    chdb_search_drop_on_abort(RelationGetRelid(index));

    bs.rw   = chdb_rowwriter_new(index);
    bs.conn = chdb_search_connect();

    PG_TRY();
    {
        char* sql = psprintf(
            "INSERT INTO %s %s FORMAT Native",
            chdb_search_table_name(RelationGetRelid(index)),
            chdb_search_column_list(index)
        );

        chdb_search_log_sql("insert", sql);
        chdb_search_insert(bs.conn, RelationGetRelid(index), sql);
        reltuples = table_index_build_scan(
            heap, index, indexInfo, true, true, build_callback, &bs, NULL
        );

        if (chdb_rowwriter_rows(bs.rw)) {
            size_t len;
            void* block = chdb_rowwriter_take(bs.rw, &len);

            chdb_search_send(bs.conn, block, len);
            pfree(block);
        }
        chdb_search_finish(bs.conn);
    }
    PG_FINALLY();
    { chdb_search_close(bs.conn); }
    PG_END_TRY();

    chdb_rowwriter_free(bs.rw);
    chdb_meta_note_flush(index);

    IndexBuildResult* result = palloc(sizeof(IndexBuildResult));

    result->heap_tuples  = reltuples;
    result->index_tuples = bs.indtuples;
    return result;
}

void
chdb_search_ambuildempty(Relation index) {
    chdb_meta_init(index, INIT_FORKNUM);
}
