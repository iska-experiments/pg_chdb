/*
 * The statements over an index's rows: a scan's SELECT, the index scan's
 * and the custom scan's alike, and the FROM clause every statement reads
 * the rows through, which unions in the transaction's staging table when
 * it has rows staged for the index. The WHERE clause and the distance
 * expressions are query.c's.
 */

#include "postgres.h"

#include "query.h"
#include "search.h"
#include "vector.h"

/* `_distance`, or `_distance3` when there are several; `_score` the same way. */
static void
append_alias(StringInfo buf, const char* name, int i, int n) {
    if (n == 1) {
        appendStringInfoString(buf, name);
    } else {
        appendStringInfo(buf, "%s%d", name, i + 1);
    }
}

/*
 * `<list> FROM <table>[ WHERE <where>[ AND xmin NOT IN (<excluded>)]]<tail>`,
 * one leg of the union a transaction that has staged rows reads.
 */
static void
append_leg(
    StringInfo buf,
    const char* list,
    const char* table,
    const char* where,
    const char* excluded,
    const char* tail
) {
    appendStringInfo(buf, "%s FROM %s", list, table);
    if (*where || excluded) {
        appendStringInfoString(buf, " WHERE ");
    }
    appendStringInfoString(buf, where);
    if (excluded) {
        appendStringInfo(buf, "%sxmin NOT IN (%s)", *where ? " AND " : "", excluded);
    }
    appendStringInfoString(buf, tail);
}

bool
chdb_search_append_from(
    StringInfo buf,
    Relation index,
    const char* where,
    const char* list,
    const char* tail
) {
    const char* excluded;
    char* table  = chdb_search_table_name(index);
    char* staged = chdb_search_staged_table(RelationGetRelid(index), &excluded);

    if (!staged) {
        appendStringInfo(buf, " FROM %s", table);
        if (*where) {
            appendStringInfo(buf, " WHERE %s", where);
        }
        return false;
    }
    list = list ? list : "SELECT *";
    tail = tail ? tail : "";
    appendStringInfoString(buf, " FROM (");
    append_leg(buf, list, table, where, NULL, tail);
    appendStringInfoString(buf, " UNION ALL ");
    append_leg(buf, list, staged, where, excluded, tail);
    appendStringInfoChar(buf, ')');
    return true;
}

char*
chdb_search_build_scored_select(
    Relation index,
    const ChdbColumn* cols,
    ScanKey keys,
    int nkeys,
    ScanKey orderbys,
    int norderbys,
    const char* const* scores,
    int nscores,
    int score_order,
    int64 limit
) {
    StringInfoData list, merged, where, tail, outer, from;
    int natts = index->rd_att->natts;

    initStringInfo(&where);
    if (!chdb_search_append_quals(&where, cols, natts, keys, nkeys)) {
        return NULL;
    }

    /*
     * One distance operator of a vector column is the search the HNSW index
     * serves, with the ORDER BY, LIMIT and SETTINGS vector.c renders.
     */
    bool knn = norderbys == 1 && !(orderbys->sk_flags & SK_ISNULL) &&
               cols[orderbys->sk_attno - 1].kind == CHDB_COL_VECTOR;

    /* The SELECT list, and the aliases alone, which a merge of two legs selects. */
    initStringInfo(&list);
    initStringInfo(&merged);
    appendStringInfoString(&list, "SELECT ctid");
    appendStringInfoString(&merged, "SELECT ctid");
    for (int i = 0; i < norderbys; i++) {
        appendStringInfo(
            &list, ", %s AS ", chdb_search_order_expr(index, cols, &orderbys[i])
        );
        append_alias(&list, "_distance", i, norderbys);
        appendStringInfoString(&merged, ", ");
        append_alias(&merged, "_distance", i, norderbys);
    }
    /* Float32, the type chdb.score() returns, so the two sides agree. */
    for (int i = 0; i < nscores; i++) {
        appendStringInfo(&list, ", toFloat32(%s) AS ", scores[i]);
        append_alias(&list, "_score", i, nscores);
        appendStringInfoString(&merged, ", ");
        append_alias(&merged, "_score", i, nscores);
    }

    /*
     * The ORDER BY and LIMIT of the statement, or of each leg of the union,
     * whose rows the merge orders and limits again by the aliases (without
     * a LIMIT when the legs of a vector search took theirs from a setting).
     */
    initStringInfo(&tail);
    initStringInfo(&outer);
    if (knn) {
        bool desc = chdb_search_vector_order(
            index, &tail, &cols[orderbys->sk_attno - 1], orderbys, limit
        );

        appendStringInfo(&outer, " ORDER BY _distance%s", desc ? " DESC" : "");
    } else if (score_order) {
        /* Ties broken by ctid, so that a LIMIT takes the same rows each time. */
        appendStringInfoString(&outer, " ORDER BY ");
        append_alias(&outer, "_score", score_order - 1, nscores);
        appendStringInfoString(&outer, " DESC, ctid");
    } else if (norderbys) {
        appendStringInfoString(&outer, " ORDER BY ");
        for (int i = 0; i < norderbys; i++) {
            appendStringInfoString(&outer, i ? ", " : "");
            append_alias(&outer, "_distance", i, norderbys);
        }
    }
    if (limit >= 0) {
        appendStringInfo(&outer, " LIMIT " INT64_FORMAT, limit);
    }
    if (!knn) {
        appendStringInfoString(&tail, outer.data);
    }

    /*
     * Each leg has its own WHERE and ORDER BY ... LIMIT, so the skip and
     * vector indexes serve both; through a plain union subquery the HNSW
     * index would not.
     */
    initStringInfo(&from);
    if (!chdb_search_append_from(&from, index, where.data, list.data, tail.data)) {
        return psprintf("%s%s%s", list.data, from.data, tail.data);
    }
    appendStringInfo(&merged, "%s%s", from.data, outer.data);
    return merged.data;
}

char*
chdb_search_build_select(
    Relation index,
    ScanKey keys,
    int nkeys,
    ScanKey orderbys,
    int norderbys,
    int64 limit
) {
    /* Once per statement; VACUUM's unqualified SELECT needs none of them. */
    ChdbColumn* cols = nkeys || norderbys ? chdb_search_columns(index) : NULL;

    return chdb_search_build_scored_select(
        index, cols, keys, nkeys, orderbys, norderbys, NULL, 0, 0, limit
    );
}
