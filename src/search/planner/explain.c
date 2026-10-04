/*
 * EXPLAIN of the custom scan: the clauses the store applies, written as the
 * query wrote them, the LIMIT it took, and the ClickHouse statement itself,
 * masked as the log masks it when chdb_search.mask_oids is on. The
 * statement needs the arguments' values, so plain EXPLAIN builds it when
 * they are constants or the statement's own parameters, and leaves it out
 * when one comes from a subplan that only ANALYZE would have run. ANALYZE
 * adds the rows the store returned, which the heap fetch may have thinned,
 * and how many times it was asked.
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

/* `a, b`: each pushed clause deparsed, as EXPLAIN shows an Order By. */
static char*
deparse_pushed(List* pushed, List* context, bool useprefix) {
    StringInfoData buf;
    ListCell* lc;

    initStringInfo(&buf);
    foreach (lc, pushed) {
        Node* clause = (Node*)((ChdbPushed*)lfirst(lc))->clause;

        appendStringInfo(
            &buf,
            "%s%s",
            buf.len ? ", " : "",
            deparse_expression(clause, context, useprefix, false)
        );
    }
    return buf.data;
}

void
chdb_planner_explain(CustomScanState* css, List* ancestors, ExplainState* es) {
    ChdbScanState* st  = (ChdbScanState*)css;
    ChdbScanSpec* spec = st->spec;
    List* context =
        set_deparse_context_plan(es->deparse_cxt, css->ss.ps.plan, ancestors);
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
    if (spec->limit >= 0) {
        ExplainPropertyInteger("Pushed Limit", NULL, spec->limit, es);
    }
    if (!st->built && !st->exec_params) {
        chdb_planner_build_sql(st, spec->limit);
    }
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
