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
#include "catalog/pg_opfamily.h"
#include "commands/vacuum.h"
#include "lib/stringinfo.h"
#include "nodes/pathnodes.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "client.h"
#include "pg-clickhouse-encode.h"
#include "pg-clickhouse.h"

/*
 * PostgreSQL 17: get_opfamily_name and the vacuum_delay_point argument came
 * with 18. commands/vacuum.h above declares the 17 form before the macro.
 */
#if PG_VERSION_NUM < 180000
#define vacuum_delay_point(is_analyze) vacuum_delay_point()
static inline char*
get_opfamily_name(Oid opfid, bool missing_ok) {
    HeapTuple tup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfid));

    if (!HeapTupleIsValid(tup)) {
        if (!missing_ok) {
            elog(ERROR, "cache lookup failed for operator family %u", opfid);
        }
        return NULL;
    }

    char* name = pstrdup(NameStr(((Form_pg_opfamily)GETSTRUCT(tup))->opfname));

    ReleaseSysCache(tup);
    return name;
}
#endif

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
 * How an indexed column is stored and searched, chosen by its operator
 * class's options support function (ddl.c), or by chdb_vector's (vector.c).
 * Classes of other extensions are columnar: their operators are still
 * rendered by strategy number.
 */
typedef enum ChdbColumnKind {
    CHDB_COL_TEXT,       /* text_ops: String with a text() skip index */
    CHDB_COL_TEXT_ARRAY, /* text_array_ops: Array(String), tokenizer = array */
    CHDB_COL_COLUMNAR,   /* columnar_ops: stored and filterable, no skip index */
    CHDB_COL_VECTOR,     /* chdb_vector's classes: Array(Float32), HNSW skip index */
} ChdbColumnKind;

/* One indexed column as it appears in the ClickHouse table. */
typedef struct ChdbColumn {
    char* name; /* ClickHouse identifier, always quoted */
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
/* `sql`, or with chdb_search.mask_oids a palloc'd copy with its numbers masked. */
extern const char*
chdb_search_mask_sql(const char* sql);

/* ---- am.c ---- */
/*
 * The index access method's cost estimate, which is also how the planner
 * hook tells a chdb index from the others: by the address of this function
 * in IndexOptInfo, whatever the access method was named.
 */
extern void
chdb_search_costestimate(
    PlannerInfo* root,
    IndexPath* path,
    double loop_count,
    Cost* indexStartupCost,
    Cost* indexTotalCost,
    Selectivity* indexSelectivity,
    double* indexCorrelation,
    double* indexPages
);

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
/* The options support functions, which ddl.c tells a column's kind by. */
extern PGDLLEXPORT Datum chdb_search_text_options(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum chdb_search_text_array_options(PG_FUNCTION_ARGS);

/* ---- textindex.c ---- */
extern char*
chdb_search_skip_index_args(Relation index, int attno, const ChdbColumn* column);
extern bool
chdb_search_wants_phrase_search(Relation index, const ChdbColumn* cols);
/* The column's tokenizer as the DDL and tokens() spell it: `ngrams(3)`. */
extern char*
chdb_search_tokenizer(Relation index, int attno, const ChdbColumn* column);
/*
 * `expr` under the column's preprocessor, as a query evaluates it: element
 * by element when `array` says the expression is one, as a text[] column is
 * and its needle is not.
 */
extern char*
chdb_search_preprocess(
    Relation index,
    int attno,
    const ChdbColumn* column,
    const char* expr,
    bool array
);

/* ---- columns.c ---- */
/* `"<name>"`, with quotes and backslashes escaped, whatever the name. */
extern char*
chdb_search_quote_ident(const char* name);
/* The kind of column an operator class with this support function 1 makes. */
extern ChdbColumnKind
chdb_search_proc_kind(Oid proc);
extern ChdbColumn*
chdb_search_columns(Relation index);

/* ---- ddl.c ---- */
/* `idx_<oid>.t_<generation>`, for the index's current generation or a given one. */
extern char*
chdb_search_table_name(Relation index);
extern char*
chdb_search_table_of(Oid indexoid, uint64 generation);
extern char*
chdb_search_create_sql(Relation index);
extern char*
chdb_search_structure(const ChdbColumn* cols, int natts);
extern char*
chdb_search_column_list(const ChdbColumn* cols, int natts);
extern void
chdb_search_create_store(Relation index);
/*
 * Run one statement on a connection of their own. `generation` is that of
 * the table the statement works on, which the worker checks exists, or zero
 * (see client.h).
 */
extern void
chdb_search_run(Oid indexoid, uint64 generation, const char* sql);
extern void
chdb_search_try_run(Oid indexoid, uint64 generation, const char* sql);
extern void
chdb_search_warn_failure(Oid indexoid);

/* ---- meta.c ---- */
/*
 * Whether this server can serve the index's store now: the server is not in
 * recovery (Phase 0 keeps the store in a local directory that base backups,
 * standbys and pg_rewind do not make current), and the store's meta table
 * agrees with the metapage on the generation and the last flush. The store
 * is asked once per (generation, flushed_lsn) a backend sees: a verdict in
 * favour is kept in rd_amcache. A scan proves this before returning any row,
 * VACUUM before deleting, a commit before flushing, the planner before
 * costing in skip mode.
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
/* A commit flushed nothing, the store being unavailable in skip mode. */
extern void
chdb_meta_note_skipped(Relation index);

/* ---- rowwriter.c, buffer.c, build.c ---- */
typedef struct ChdbRowWriter ChdbRowWriter;

extern ChdbRowWriter*
chdb_rowwriter_new(Relation index);
/* `(ctid, xmin, ...)` for the INSERT the writer's blocks go into. */
extern char*
chdb_rowwriter_column_list(ChdbRowWriter* w);
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
 * for a scan `SELECT ctid[, distances][, scores]`, int8 (the packed TID),
 * then float8s, then float4s.
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
chdb_search_stream_open(
    Oid indexoid,
    uint64 generation,
    const char* sql,
    int ndist,
    int nscores,
    MemoryContext cxt
);
extern ChdbStream*
chdb_search_stream_query(
    Oid indexoid,
    uint64 generation,
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
/*
 * Whether `tid` names a block the heap has, so that a store holding blocks
 * past the heap's end never makes the heap fetch raise; *nblocks caches the
 * heap's size between calls and starts at zero.
 */
extern bool
chdb_search_tid_in_heap(Relation heap, BlockNumber* nblocks, ItemPointer tid);

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
