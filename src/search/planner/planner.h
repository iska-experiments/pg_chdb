#ifndef CHDB_SEARCH_PLANNER_H
#define CHDB_SEARCH_PLANNER_H

/*
 * The chdb_search custom scan: one ClickHouse query for a base relation with
 * a chdb index, where an index scan would run the same query and then pay
 * the access method's per-tuple overhead on top. A set_rel_pathlist hook
 * (hook.c) adds a CustomPath when the relation's restriction clauses hold a
 * chdb predicate, as an operator or as a function call, or the query's
 * ORDER BY is a distance of an indexed column; plan.c turns the path into a
 * CustomScan, exec.c runs it and explain.c describes it.
 *
 * What goes to ClickHouse is a list of ChdbPushed, each the planner's view of
 * one ScanKey: the index column, the strategy number of the operator in the
 * column's operator family and the argument expression, which the executor
 * evaluates when the scan starts and renders through query.c, exactly as an
 * index scan's keys are rendered. Predicates become the WHERE clause,
 * orders the `AS _distance` columns and the ORDER BY; a LIMIT the query can
 * take goes with them. The rows come back as ctids, which the scan fetches
 * from the heap under the executor's snapshot, so visibility is Postgres's.
 *
 * chdb.score(k) is a column the store computes (score.c here binds it, the
 * access method's score.c renders it): the spec's outputs, selected after
 * the distances, which plan.c names in a custom_scan_tlist behind the heap
 * columns the query needs, so that setrefs.c points the target list and the
 * quals at them, and exec.c fills a virtual scan tuple from the heap row and
 * the stream.
 *
 * The aggregate scan (agg_*.c) is the stage for GROUP BY and the aggregates
 * the store computes: a create_upper_paths hook builds a spec whose
 * agg_outputs are the grouping columns and the aggregates (ChdbAggOutput),
 * the plan is a scan with no relation, scanrelid 0, whose custom_scan_tlist
 * names them, and the executor fills a virtual scan tuple from the store's
 * groups, or from the Agg plan it carries as a child when the heap cannot
 * vouch for the store's answer.
 */

#include "postgres.h"

#include "access/skey.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

struct ChdbColumn; /* search.h */
struct ChdbStream; /* stream.h */

/* One expression the scan sends to ClickHouse rather than evaluating. */
typedef struct ChdbPushed {
    AttrNumber attno;        /* index column, 1-based */
    StrategyNumber strategy; /* of the operator in the column's family */
    Oid subtype;             /* the argument's type, the key's sk_subtype */
    Oid collation;           /* the operator's input collation */
    int argno;               /* which argument of the clause is not the column */
    Expr* clause;            /* the clause as written, for EXPLAIN and rechecks */
    RestrictInfo* rinfo;     /* at planning, the restriction it came from, or NULL */
} ChdbPushed;

/* The argument of a pushed clause, the side that is not the column. */
extern Expr*
chdb_pushed_arg(const ChdbPushed* p);

/* A column the store computes for the query: chdb.score(k[, 'col']). */
typedef struct ChdbOutput {
    Expr* expr;       /* the call as written, which the scan tuple stands for */
    AttrNumber attno; /* the index column it is restricted to, or 0 for all */
} ChdbOutput;

/* What one column of an aggregate scan's output is. */
typedef enum ChdbAggKind {
    CHDB_AGG_GROUP,     /* a GROUP BY column, returned as stored */
    CHDB_AGG_COUNT,     /* count(*) */
    CHDB_AGG_COUNT_COL, /* count(col): the rows where it is not NULL */
    CHDB_AGG_MIN,
    CHDB_AGG_MAX,
    CHDB_AGG_SUM,
    CHDB_AGG_AVG, /* the store's sum and count, divided here as Postgres does */
} ChdbAggKind;

typedef struct ChdbAggOutput {
    ChdbAggKind kind;
    AttrNumber attno;   /* index column, 1-based; 0 for count(*) */
    Oid typid;          /* the Postgres type of the output */
    Expr* expr;         /* the Var or Aggref it stands for: the scan tuple's column */
    Index sortgroupref; /* at planning, the GROUP BY clause it belongs to, or 0 */
} ChdbAggOutput;

/* What one scan sends: shared by the path, the plan and the executor. */
typedef struct ChdbScanSpec {
    Oid indexoid;
    List* quals;       /* ChdbPushed, ANDed into the WHERE clause */
    List* orderbys;    /* ChdbPushed, the ORDER BY in order */
    List* outputs;     /* ChdbOutput, selected after the distances */
    int score_order;   /* 1-based output the rows are ordered by, or 0 */
    int64 limit;       /* LIMIT the query takes, negative for none */
    List* agg_outputs; /* ChdbAggOutput, an aggregate scan's SELECT list; else NIL */
} ChdbScanSpec;

/* ---- match.c: clauses and pathkeys to pushed expressions ---- */

/* Through the no-op relabelings the planner wraps binary-coercible types in. */
extern Node*
chdb_planner_strip(Node* node);
/* The index column `node` is a plain Var of, 1-based, or 0. */
extern AttrNumber
chdb_planner_index_column(RelOptInfo* rel, IndexOptInfo* index, Node* node);

/*
 * Whether `rinfo` is a predicate ClickHouse can apply on a column of
 * `index`: an operator of the column's family, or a call of such an
 * operator's function, between the column and an expression free of the
 * relation's columns and of volatile functions. Fills *out.
 */
extern bool
chdb_planner_match_clause(
    PlannerInfo* root,
    RelOptInfo* rel,
    IndexOptInfo* index,
    RestrictInfo* rinfo,
    ChdbPushed* out
);

/*
 * Whether `pathkey` sorts ascending by a distance operator of the column's
 * family applied to an indexed column and such an expression. Fills *out.
 */
extern bool
chdb_planner_match_pathkey(
    PlannerInfo* root,
    RelOptInfo* rel,
    IndexOptInfo* index,
    PathKey* pathkey,
    ChdbPushed* out
);

/* ---- score.c: chdb.score() calls to outputs ---- */

/*
 * The chdb.score() calls the query evaluates on `rel`, in its target list,
 * its HAVING clause and the relation's restrictions, each once, with the
 * column a second argument names resolved to an index column of `index`.
 * None for a statement that may recheck rows: EvalPlanQual hands the scan
 * a heap tuple, which the scan tuple the outputs need is not.
 */
extern List*
chdb_planner_collect_scores(PlannerInfo* root, RelOptInfo* rel, IndexOptInfo* index);

/* Whether `pathkey` sorts by one of `outputs` descending; *n is its 1-based place. */
extern bool
chdb_planner_match_score_pathkey(PathKey* pathkey, List* outputs, int* n);

/* ---- hook.c: the path ---- */

extern bool chdb_search_enable_custom_scan;
extern double chdb_search_custom_scan_cost_factor;

/* Defines the GUCs and installs the hook; run by _PG_init. */
extern void
chdb_search_planner_init(void);

typedef struct ChdbPath {
    CustomPath cpath;
    ChdbScanSpec spec;
    IndexOptInfo* index;
    List* local;  /* RestrictInfo, the clauses the scan applies itself */
    List* having; /* an aggregate scan: the HAVING clauses it applies to its output */
} ChdbPath;

/* Whether the hook plans scans of `rel`: a plain heap table with an index. */
extern bool
chdb_planner_eligible_rel(RelOptInfo* rel, RangeTblEntry* rte);
/* Whether `index` is a chdb index whose store a scan may use now. */
extern bool
chdb_planner_usable_index(IndexOptInfo* index);
/*
 * Sorts the clauses of `rel` into spec.quals, the ones the store applies, by
 * index column, and `local`, the ones the scan applies itself; true when
 * any pushed one is a text search.
 */
extern bool
chdb_planner_collect_quals(PlannerInfo* root, RelOptInfo* rel, ChdbPath* p);

/* ---- agg_hook.c: the aggregate path ---- */

extern bool chdb_search_enable_aggregate_pushdown;

/* Defines the GUC and installs the hook; run by chdb_search_planner_init. */
extern void
chdb_planner_aggregate_init(void);
/* Whether the visibility map says every page of `heap` is all-visible. */
extern bool
chdb_planner_heap_all_visible(Relation heap);

/* ---- agg_match.c: the grouped target to outputs ---- */

/*
 * Whether the store can compute `exprs`, the grouped target and the HAVING
 * clauses: every Var a GROUP BY column of the index, every Aggref one of
 * count, min, max, sum and avg over an index column. Fills *outputs with
 * the GROUP BY columns first.
 */
extern bool
chdb_planner_match_aggregates(
    PlannerInfo* root,
    RelOptInfo* rel,
    IndexOptInfo* index,
    const struct ChdbColumn* cols,
    List* exprs,
    List** outputs
);

/* ---- cost.c ---- */

extern void
chdb_planner_cost(PlannerInfo* root, RelOptInfo* rel, ChdbPath* path);
extern void
chdb_planner_cost_aggregate(PlannerInfo* root, ChdbPath* path, double ngroups);

/* ---- plan.c: the CustomScan and its private data ---- */

extern const CustomPathMethods chdb_planner_path_methods;
/*
 * A CustomScan of `p` with the parts every chdb scan has: the target list,
 * the quals, the pushed clauses in custom_exprs and the spec's numbers in
 * custom_private. The caller sets what differs.
 */
extern CustomScan*
chdb_planner_make_scan(
    PlannerInfo* root,
    ChdbPath* p,
    List* tlist,
    List* qual,
    List* custom_plans
);
extern List*
chdb_planner_pack_int(List* list, int64 value);
/* The next Integer of custom_private. */
extern int
chdb_planner_unpack_int(ListCell** lc, List* list);
/* The spec a plan carries, with the clauses from its custom_exprs. */
extern ChdbScanSpec*
chdb_planner_unpack(const CustomScan* cscan);
/* The same, leaving *lc at what custom_private holds after the spec. */
extern ChdbScanSpec*
chdb_planner_unpack_at(const CustomScan* cscan, ListCell** lc);

/* ---- agg_plan.c ---- */

extern const CustomPathMethods chdb_planner_agg_path_methods;
extern const CustomScanMethods chdb_planner_agg_scan_methods;
extern ChdbScanSpec*
chdb_planner_agg_unpack(const CustomScan* cscan);

/* ---- sql.c, exec.c, explain.c ---- */

extern const CustomScanMethods chdb_planner_scan_methods;
extern const CustomExecMethods chdb_planner_exec_methods;

typedef struct ChdbScanState {
    CustomScanState css;
    ChdbScanSpec* spec;
    Relation index;
    MemoryContext cxt; /* the statement, the keys and the stream */
    List* args;        /* ExprState per pushed expression, quals then orderbys */
    ExprState* recheck;
    struct IndexFetchTableData* fetch;
    TupleTableSlot* heap_slot; /* the fetched row, when the scan tuple is virtual */
    AttrNumber* attnos; /* heap attribute per scan tuple column before the outputs */
    int nvars;
    struct ChdbScoreCache* scores; /* the counts behind the outputs, per statement */
    struct ChdbStream* stream;
    struct HTAB* seen; /* ctids returned so far, when a LIMIT is pushed */
    char* sql;         /* NULL once built when a search key is NULL: no rows */
    bool built;
    int64 asked;      /* LIMIT of the running query */
    int64 got;        /* rows the running query returned */
    int64 returned;   /* visible rows returned so far */
    int64 store_rows; /* for EXPLAIN ANALYZE */
    int store_queries;
    bool started;
    bool skip;        /* the store is unavailable and the GUC says skip */
    bool exec_params; /* an argument comes from a subplan */
    BlockNumber heap_nblocks;
} ChdbScanState;

/* ExprStates for the pushed arguments, quals then orderbys, and the recheck. */
extern void
chdb_planner_init_args(ChdbScanState* st);
/*
 * The pushed expressions as scan keys, quals then orderbys, their arguments
 * evaluated in the per-tuple memory: render them before the next row.
 */
extern ScanKey
chdb_planner_scan_keys(ChdbScanState* st);
/* Builds the statement from the evaluated arguments; for EXPLAIN too. */
extern void
chdb_planner_build_sql(ChdbScanState* st, int64 limit);
/* Forgets the statement and its rows; the arguments may change on a rescan. */
extern void
chdb_planner_reset(ChdbScanState* st);

struct ExplainState;
extern void
chdb_planner_explain(CustomScanState* css, List* ancestors, struct ExplainState* es);

/* ---- agg_select.c, agg_exec.c: the aggregate scan ---- */

typedef struct ChdbAggState {
    ChdbScanState scan; /* the spec, the index, the arguments, the statement */
    Relation heap;
    struct PlanState* exact; /* the Agg plan that answers when the store cannot */
    struct Tuplestorestate*
        rows;             /* the store's groups, until the heap is checked again */
    const char* fallback; /* why the exact plan answers, or NULL */
} ChdbAggState;

extern const CustomExecMethods chdb_planner_agg_exec_methods;

/* The aggregate statement from the evaluated arguments; for EXPLAIN too. */
extern void
chdb_planner_build_agg_sql(ChdbScanState* st);
/* The Postgres types the statement's columns decode to; AVG takes two. */
extern Oid*
chdb_planner_agg_types(const ChdbScanSpec* spec, int* ncols);
/* The stream's current row as the scan tuple. */
extern void
chdb_planner_agg_fill(
    const ChdbScanSpec* spec,
    const struct ChdbStream* s,
    TupleTableSlot* slot
);
/* Nulls the scan tuple's columns from `from` on, the ones no output fills. */
extern void
chdb_planner_agg_pad(TupleTableSlot* slot, int from);
extern void
chdb_planner_agg_explain(
    CustomScanState* css,
    List* ancestors,
    struct ExplainState* es
);

#endif /* CHDB_SEARCH_PLANNER_H */
