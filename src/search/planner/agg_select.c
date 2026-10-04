/*
 * The statement an aggregate scan sends and the shape of its answer. Each
 * output is one ClickHouse expression, two for avg, over the index's
 * columns as columns.c names them; the WHERE clause is the pushed quals
 * rendered by query.c, as every chdb statement's is, and the GROUP BY the
 * grouping columns. The answer's columns decode to the Postgres types the
 * outputs have, and a row of them becomes the scan tuple.
 *
 * The aggregates are the OrNull forms, so that min, max, sum and avg of no
 * rows are NULL as in Postgres rather than ClickHouse's zero. A sum is
 * typed as Postgres types it: of int2 and int4 an Int64, of int8 an Int128
 * (Postgres's numeric sum does not overflow where ClickHouse's Int64
 * would), of float4 a Float32, and of numeric the column's Decimal. avg is
 * the store's sum and count divided here with the same functions Postgres's
 * aggregate uses, numeric_div or a float division, so the result has the
 * digits Postgres's would.
 */

#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "utils/fmgrprotos.h"
#include "utils/memutils.h"
#include "utils/numeric.h"

#include "../query.h"
#include "../search.h"
#include "../stream.h"
#include "planner.h"

/* sumOrNull of the column, as a Float32 when Postgres's sum is a float4. */
static void
append_sum(StringInfo buf, const ChdbColumn* col, bool as_float4) {
    if (col->typid == INT8OID) {
        appendStringInfo(buf, "sumOrNull(toInt128(%s))", col->name);
    } else if (as_float4) {
        appendStringInfo(buf, "toFloat32(sumOrNull(%s))", col->name);
    } else {
        appendStringInfo(buf, "sumOrNull(%s)", col->name);
    }
}

static void
append_output(StringInfo buf, const ChdbAggOutput* o, const ChdbColumn* col) {
    switch (o->kind) {
    case CHDB_AGG_GROUP:
        appendStringInfoString(buf, col->name);
        return;
    case CHDB_AGG_COUNT:
        appendStringInfoString(buf, "count()");
        return;
    case CHDB_AGG_COUNT_COL:
        appendStringInfo(buf, "count(%s)", col->name);
        return;
    case CHDB_AGG_MIN:
        appendStringInfo(buf, "minOrNull(%s)", col->name);
        return;
    case CHDB_AGG_MAX:
        appendStringInfo(buf, "maxOrNull(%s)", col->name);
        return;
    case CHDB_AGG_SUM:
        append_sum(buf, col, o->typid == FLOAT4OID);
        return;
    case CHDB_AGG_AVG:
        append_sum(buf, col, false);
        appendStringInfo(buf, ", count(%s)", col->name);
        return;
    }
    elog(ERROR, "unknown chdb aggregate output kind %d", o->kind);
}

void
chdb_planner_build_agg_sql(ChdbScanState* st) {
    Relation index    = st->index;
    int natts         = index->rd_att->natts;
    int nquals        = list_length(st->spec->quals);
    ScanKey keys      = chdb_planner_scan_keys(st);
    MemoryContext old = MemoryContextSwitchTo(st->cxt);
    ChdbColumn* cols  = chdb_search_columns(index);
    StringInfoData buf, where, group;
    ListCell* lc;

    initStringInfo(&buf);
    initStringInfo(&where);
    initStringInfo(&group);
    appendStringInfoString(&buf, "SELECT ");
    foreach (lc, st->spec->agg_outputs) {
        ChdbAggOutput* o      = lfirst(lc);
        const ChdbColumn* col = o->kind == CHDB_AGG_COUNT ? NULL : &cols[o->attno - 1];

        if (lc != list_head(st->spec->agg_outputs)) {
            appendStringInfoString(&buf, ", ");
        }
        append_output(&buf, o, col);
        if (o->kind == CHDB_AGG_GROUP) {
            appendStringInfo(&group, "%s%s", group.len ? ", " : "", col->name);
        }
    }
    appendStringInfo(&buf, " FROM %s", chdb_search_table_name(index));
    /* The operators are strict: a NULL argument matches no row. */
    if (!chdb_search_append_quals(&where, cols, natts, keys, nquals)) {
        appendStringInfoString(&buf, " WHERE 0");
    } else if (where.len) {
        appendStringInfo(&buf, " WHERE %s", where.data);
    }
    if (group.len) {
        appendStringInfo(&buf, " GROUP BY %s", group.data);
    }
    MemoryContextSwitchTo(old);
    st->sql   = buf.data;
    st->built = true;
}

Oid*
chdb_planner_agg_types(const ChdbScanSpec* spec, int* ncols) {
    Oid* types = palloc(sizeof(Oid) * 2 * list_length(spec->agg_outputs));
    ListCell* lc;

    *ncols = 0;
    foreach (lc, spec->agg_outputs) {
        ChdbAggOutput* o = lfirst(lc);

        switch (o->kind) {
        case CHDB_AGG_COUNT:
        case CHDB_AGG_COUNT_COL:
            types[(*ncols)++] = INT8OID;
            break;
        case CHDB_AGG_AVG:
            types[(*ncols)++] = o->typid;
            types[(*ncols)++] = INT8OID;
            break;
        default:
            types[(*ncols)++] = o->typid;
        }
    }
    return types;
}

/* sum / count as Postgres's avg divides them; NULL when nothing was summed. */
static Datum
average(Oid typid, Datum sum, bool sumnull, int64 count, bool* isnull) {
    *isnull = sumnull || count == 0;
    if (*isnull) {
        return (Datum)0;
    }
    if (typid == FLOAT8OID) {
        return Float8GetDatum(DatumGetFloat8(sum) / (double)count);
    }
    return DirectFunctionCall2(
        numeric_div, sum, NumericGetDatum(int64_to_numeric(count))
    );
}

/* The scan tuple's columns past the outputs, which the pushed clauses name: NULL. */
void
chdb_planner_agg_pad(TupleTableSlot* slot, int from) {
    for (int i = from; i < slot->tts_tupleDescriptor->natts; i++) {
        slot->tts_values[i] = (Datum)0;
        slot->tts_isnull[i] = true;
    }
}

void
chdb_planner_agg_fill(
    const ChdbScanSpec* spec,
    const ChdbStream* s,
    TupleTableSlot* slot
) {
    ListCell* lc;
    int c = 0, i = 0;

    ExecClearTuple(slot);
    foreach (lc, spec->agg_outputs) {
        ChdbAggOutput* o = lfirst(lc);

        if (o->kind == CHDB_AGG_AVG) {
            slot->tts_values[i] = average(
                o->typid,
                s->vals[c],
                s->nulls[c],
                DatumGetInt64(s->vals[c + 1]),
                &slot->tts_isnull[i]
            );
            c += 2;
        } else {
            slot->tts_values[i] = s->vals[c];
            slot->tts_isnull[i] = s->nulls[c];
            c++;
        }
        i++;
    }
    chdb_planner_agg_pad(slot, i);
    ExecStoreVirtualTuple(slot);
}
