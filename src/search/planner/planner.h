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
 * A later stage that computes a column in the store (chdb.score) adds its
 * expression to a spec as another ChdbPushed kind and its value to the
 * stream after the distances; plan.c then needs a custom_scan_tlist naming
 * it and exec.c a virtual scan tuple to put it in. An aggregate stage builds
 * a spec of its own from create_upper_paths_hook and a scan with no
 * relation, scanrelid 0, returning the store's row as the scan tuple.
 */

#include "postgres.h"

#include "access/skey.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

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

/* What one scan sends: shared by the path, the plan and the executor. */
typedef struct ChdbScanSpec {
    Oid indexoid;
    List* quals;    /* ChdbPushed, ANDed into the WHERE clause */
    List* orderbys; /* ChdbPushed, the ORDER BY in order */
    int64 limit;    /* LIMIT the query takes, negative for none */
} ChdbScanSpec;

/* ---- match.c: clauses and pathkeys to pushed expressions ---- */

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
    List* local; /* RestrictInfo, the clauses the scan applies itself */
} ChdbPath;

/* ---- cost.c ---- */

extern void
chdb_planner_cost(PlannerInfo* root, RelOptInfo* rel, ChdbPath* path);

/* ---- plan.c: the CustomScan and its private data ---- */

extern const CustomPathMethods chdb_planner_path_methods;
/* The spec a plan carries, with the clauses from its custom_exprs. */
extern ChdbScanSpec*
chdb_planner_unpack(const CustomScan* cscan);

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

/* Builds the statement from the evaluated arguments; for EXPLAIN too. */
extern void
chdb_planner_build_sql(ChdbScanState* st, int64 limit);

struct ExplainState;
extern void
chdb_planner_explain(CustomScanState* css, List* ancestors, struct ExplainState* es);

#endif /* CHDB_SEARCH_PLANNER_H */
