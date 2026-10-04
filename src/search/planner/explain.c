/*
 * EXPLAIN of the custom scans: the clauses the store applies, written as the
 * query wrote them, the scores it computes, the LIMIT it took, and the
 * ClickHouse statement itself, masked as the log masks it when
 * chdb_search.mask_oids is on. The statement needs the arguments' values,
 * so plain EXPLAIN builds it when they are constants or the statement's own
 * parameters, and leaves it out when one comes from a subplan that only
 * ANALYZE would have run; with a score it also asks the store for the
 * counts behind the statement's weights. ANALYZE adds the rows the store
 * returned, which the heap fetch may have thinned, and how many times it
 * was asked; for the aggregate scan, why the exact plan answered instead,
 * when it did.
 */

#include "postgres.h"

#include "commands/explain.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#endif
#include "nodes/makefuncs.h"
#include "utils/ruleutils.h"

#include "../search.h"
#include "planner.h"

/* `a, b`: each expression deparsed, as EXPLAIN shows an Order By. */
static char*
deparse_list(List* exprs, List* context, bool useprefix) {
    StringInfoData buf;
    ListCell* lc;

    initStringInfo(&buf);
    foreach (lc, exprs) {
        appendStringInfo(
            &buf,
            "%s%s",
            buf.len ? ", " : "",
            deparse_expression(lfirst(lc), context, useprefix, false)
        );
    }
    return buf.data;
}

static char*
deparse_pushed(List* pushed, List* context, bool useprefix) {
    List* clauses = NIL;
    ListCell* lc;

    foreach (lc, pushed) {
        clauses = lappend(clauses, ((ChdbPushed*)lfirst(lc))->clause);
    }
    return deparse_list(clauses, context, useprefix);
}

static char*
deparse_outputs(List* outputs, List* context, bool useprefix) {
    List* exprs = NIL;
    ListCell* lc;

    foreach (lc, outputs) {
        exprs = lappend(exprs, ((ChdbOutput*)lfirst(lc))->expr);
    }
    return deparse_list(exprs, context, useprefix);
}

/* What was pushed: the clauses, the order, the scores and the LIMIT. */
static void
explain_pushed(ChdbScanState* st, List* ancestors, ExplainState* es) {
    ChdbScanSpec* spec = st->spec;
    List* context =
        set_deparse_context_plan(es->deparse_cxt, st->css.ss.ps.plan, ancestors);
    bool useprefix = list_length(es->rtable) > 1 || es->verbose;

    if (spec->quals) {
        ExplainPropertyText(
            "Pushed Cond", deparse_pushed(spec->quals, context, useprefix), es
        );
    }
    if (spec->orderbys) {
        ExplainPropertyText(
            "Pushed Order By", deparse_pushed(spec->orderbys, context, useprefix), es
        );
    }
    if (spec->outputs) {
        ExplainPropertyText(
            "Pushed Score", deparse_outputs(spec->outputs, context, useprefix), es
        );
    }
    if (spec->limit >= 0) {
        ExplainPropertyInteger("Pushed Limit", NULL, spec->limit, es);
    }
}

/* The statement, once built, and under ANALYZE what the store returned. */
static void
explain_statement(ChdbScanState* st, ExplainState* es) {
    if (st->built) {
        ExplainPropertyText(
            "ClickHouse",
            st->sql ? chdb_search_mask_sql(st->sql) : "none: a search key is NULL",
            es
        );
    }
    if (es->analyze) {
        ExplainPropertyInteger("Store Rows", NULL, st->store_rows, es);
        if (st->store_queries > 1) {
            ExplainPropertyInteger("Store Queries", NULL, st->store_queries, es);
        }
    }
}

void
chdb_planner_explain(CustomScanState* css, List* ancestors, ExplainState* es) {
    ChdbScanState* st = (ChdbScanState*)css;

    explain_pushed(st, ancestors, es);
    if (!st->built && !st->exec_params) {
        chdb_planner_build_sql(st, st->spec->limit);
    }
    explain_statement(st, es);
}

void
chdb_planner_agg_explain(CustomScanState* css, List* ancestors, ExplainState* es) {
    ChdbAggState* a   = (ChdbAggState*)css;
    ChdbScanState* st = &a->scan;

    explain_pushed(st, ancestors, es);
    if (!st->built && !st->exec_params) {
        chdb_planner_build_agg_sql(st);
    }
    explain_statement(st, es);
    if (es->analyze && a->fallback) {
        ExplainPropertyText("Exact Plan", a->fallback, es);
    }
}
