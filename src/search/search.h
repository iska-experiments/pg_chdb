#ifndef CHDB_SEARCH_H
#define CHDB_SEARCH_H

/*
 * Internal interface of the chdb index access method. The ClickHouse side is
 * reached through client.h only; query.h is the part other sub-projects use.
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/itup.h"
#include "access/reloptions.h"
#include "lib/stringinfo.h"
#include "utils/rel.h"

#include "client.h"
#include "pg-clickhouse-encode.h"
#include "pg-clickhouse.h"

/* GUCs, defined in gucs.c. */
extern int chdb_search_flush_threshold_kb;
extern int chdb_search_max_buffer_kb;
extern double chdb_search_vacuum_optimize_ratio;
extern bool chdb_search_mask_oids;

/* chdb_search.unavailable_index: what a scan does with a store it cannot trust. */
typedef enum ChdbUnavailableAction {
    CHDB_UNAVAILABLE_ERROR, /* raise, so a broken index is never silent */
    CHDB_UNAVAILABLE_SKIP,  /* let the planner use another path */
} ChdbUnavailableAction;
extern int chdb_search_unavailable_index;

/*
 * How an indexed column is stored and searched, chosen by its operator class.
 * Unknown operator classes (added by other extensions) are treated as columnar.
 */
typedef enum ChdbColumnKind {
    CHDB_COL_TEXT,       /* text_ops: String with a text() skip index */
    CHDB_COL_TEXT_ARRAY, /* text_array_ops: Array(String), tokenizer = array */
    CHDB_COL_COLUMNAR,   /* columnar_ops: stored and filterable, no skip index */
} ChdbColumnKind;

/* One indexed column as it appears in the ClickHouse table. */
typedef struct ChdbColumn {
    char* name; /* ClickHouse identifier, quoted when needed */
    char* type; /* ClickHouse type */
    ChdbColumnKind kind;
    Oid typid; /* Postgres type of the index attribute */
} ChdbColumn;

/* ---- gucs.c ---- */
/* GUCs, reloptions and transaction callbacks; run by _PG_init. */
extern void
chdb_search_am_init(void);
extern void
chdb_search_log_sql(const char* what, const char* sql);

/* ---- validate.c ---- */
extern bool
chdb_search_validate(Oid opclassoid);

/* ---- options.c ---- */
extern void
chdb_search_init_options(void);
extern bytea*
chdb_search_amoptions(Datum reloptions, bool validate);
extern double
chdb_search_index_optimize_ratio(Relation index);
extern char*
chdb_search_skip_index_args(
    Relation index,
    int attno,
    const char* col,
    ChdbColumnKind kind
);

extern bool
chdb_search_wants_phrase_search(Relation index);

/*
 * The store generation every request names (client.h), which the engine
 * checks a table idx_<oid>.t_<generation> against: the store holds one table,
 * idx_<oid>.t, named after no generation, so zero, which asks for no check.
 */
#define CHDB_SEARCH_NO_GENERATION 0

/* ---- ddl.c ---- */
extern ChdbColumn*
chdb_search_columns(Relation index);
extern char*
chdb_search_table_name(Relation index);
extern char*
chdb_search_create_sql(Relation index);
extern char*
chdb_search_structure(Relation index);
extern char*
chdb_search_column_list(Relation index);
extern void
chdb_search_create_store(Relation index);
extern void
chdb_search_run(Oid indexoid, const char* sql);
extern void
chdb_search_try_run(Oid indexoid, const char* sql);
extern void
chdb_search_warn_failure(Oid indexoid);

/* ---- meta.c ---- */
/*
 * Whether this server can serve the index's store now. Phase 0 keeps the
 * store in a local directory that base backups, standbys and pg_rewind do
 * not make current, so a server in recovery has no store it can trust; the
 * generation and LSN comparison against the worker (follow-up) will refine
 * this. A scan proves this true before returning any row.
 */
extern bool
chdb_search_store_unavailable(Relation index);
/*
 * Acts on an unavailable store per chdb_search.unavailable_index: raises in
 * error mode, or sets *skip so the caller yields no rows. *skip is false and
 * nothing is raised when the store is available.
 */
extern void
chdb_search_check_available(Relation index, bool* skip);

#define CHDB_META_MAGIC 0x43484453 /* "CHDS" */
#define CHDB_META_VERSION 1
#define CHDB_METAPAGE_BLKNO 0

typedef struct ChdbMetaPageData {
    uint32 magic;
    uint32 version;
    uint64 generation;  /* random per build, ties the store to this relation */
    uint64 flushed_lsn; /* WAL position when the store was last written */
} ChdbMetaPageData;

extern uint64
chdb_meta_init(Relation index, ForkNumber fork);
extern void
chdb_meta_read(Relation index, ChdbMetaPageData* out);
extern uint64
chdb_meta_generation(Relation index);
extern void
chdb_meta_note_flush(Relation index);

/* ---- rowwriter.c, buffer.c, build.c ---- */
typedef struct ChdbRowWriter ChdbRowWriter;

extern ChdbRowWriter*
chdb_rowwriter_new(Relation index);
extern void
chdb_rowwriter_append(
    ChdbRowWriter* w,
    ItemPointer tid,
    TransactionId xmin,
    Datum* values,
    bool* isnull
);
extern void
chdb_rowwriter_checkpoint(ChdbRowWriter* w, pgch_checkpoint* ckpt);
extern void
chdb_rowwriter_rollback(ChdbRowWriter* w, const pgch_checkpoint* ckpt);
extern void
chdb_rowwriter_revalidate(ChdbRowWriter* w, pgch_checkpoint* ckpt);
extern size_t
chdb_rowwriter_bytes(ChdbRowWriter* w);
extern size_t
chdb_rowwriter_rows(ChdbRowWriter* w);
/* Serializes the buffered rows as one Native block into a palloc'd buffer. */
extern void*
chdb_rowwriter_take(ChdbRowWriter* w, size_t* len);
extern void
chdb_rowwriter_free(ChdbRowWriter* w);

extern uint64
chdb_search_tid_to_u64(ItemPointer tid);
extern void
chdb_search_u64_to_tid(uint64 v, ItemPointer tid);

extern void
chdb_search_init_insert(void);
/* A rebuild of the index takes over the rows buffered for it. */
extern void
chdb_search_discard_pending(Oid indexoid);
extern bool
chdb_search_aminsert(
    Relation index,
    Datum* values,
    bool* isnull,
    ItemPointer ht_ctid,
    Relation heap,
    IndexUniqueCheck checkUnique,
    bool indexUnchanged,
    struct IndexInfo* indexInfo
);
extern IndexBuildResult*
chdb_search_ambuild(Relation heap, Relation index, struct IndexInfo* indexInfo);
extern void
chdb_search_ambuildempty(Relation index);

/* ---- drop.c ---- */
extern void
chdb_search_init_drop(void);
extern void
chdb_search_drop_on_abort(Oid indexoid);
extern void
chdb_search_drop_statement_on_abort(Oid indexoid, const char* sql);
extern void
chdb_search_forget_statement(Oid indexoid, const char* sql);

/* ---- scan.c ---- */
#include "pg-clickhouse-decode.h"

/*
 * A running SELECT against the worker, each column decoded to a Postgres type:
 * for a scan `SELECT ctid[, distances]`, int8 (the packed TID) then float8s.
 */
typedef struct ChdbStream {
    chdbSearchConn* conn;
    pgch_reader reader;
    void** states;
    Datum* vals;
    bool* nulls;
    int ncols;
    MemoryContext cxt;
    MemoryContext rowcxt;
    bool done;
} ChdbStream;

extern ChdbStream*
chdb_search_stream_open(Oid indexoid, const char* sql, int ndist, MemoryContext cxt);
extern ChdbStream*
chdb_search_stream_query(
    Oid indexoid,
    const char* sql,
    const Oid* types,
    int ncols,
    MemoryContext cxt
);
/*
 * Next row into s->vals, valid until the next call; false at the end, which
 * also closes the stream. `tid`, if given, takes the packed ctid of column 0.
 */
extern bool
chdb_search_stream_next(ChdbStream* s, ItemPointer tid);
/* Abandons the stream, finishing it only if it ran to the end. */
extern void
chdb_search_stream_close(ChdbStream* s);

/* ---- scan.c, vacuum.c ---- */
extern IndexScanDesc
chdb_search_ambeginscan(Relation index, int nkeys, int norderbys);
extern void
chdb_search_amrescan(
    IndexScanDesc scan,
    ScanKey keys,
    int nkeys,
    ScanKey orderbys,
    int norderbys
);
extern bool
chdb_search_amgettuple(IndexScanDesc scan, ScanDirection dir);
extern int64
chdb_search_amgetbitmap(IndexScanDesc scan, struct TIDBitmap* tbm);
extern void
chdb_search_amendscan(IndexScanDesc scan);

extern IndexBulkDeleteResult*
chdb_search_ambulkdelete(
    IndexVacuumInfo* info,
    IndexBulkDeleteResult* stats,
    IndexBulkDeleteCallback callback,
    void* callback_state
);
extern IndexBulkDeleteResult*
chdb_search_amvacuumcleanup(IndexVacuumInfo* info, IndexBulkDeleteResult* stats);

#endif /* CHDB_SEARCH_H */
