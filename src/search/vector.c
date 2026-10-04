/*
 * Vector columns, by the contract of ../vector/chdb_vector.h: a column whose
 * operator class names chdb_vector's support functions is stored as
 * Array(Float32) under an HNSW skip index, and a scan ordered by its
 * distance operator asks ClickHouse for the nearest rows through that
 * index. Nothing here names a pgvector type or function: the class is told
 * by its support function, the ClickHouse function by what that answers,
 * and the encoder casts each vector through pgvector's own cast to real[].
 */

#include "postgres.h"

#include <string.h>

#include "access/htup_details.h"
#include "catalog/pg_amop.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "../vector/chdb_vector.h"
#include "query.h"
#include "search.h"
#include "vector.h"

bool
chdb_search_is_vector(Relation index, int attno) {
    return OidIsValid(index_getprocid(index, attno, CHDB_VECTOR_PROC_DISTANCE_NAME));
}

/* What support function `proc` of the column answers for a strategy. */
static char*
ask(Relation index, int attno, uint16 proc, StrategyNumber strategy) {
    FmgrInfo* fn = index_getprocinfo(index, attno, proc);
    Datum d      = FunctionCall1Coll(fn, InvalidOid, Int16GetDatum(strategy));

    return text_to_cstring(DatumGetTextPP(d));
}

/*
 * The dimension the column declares, which pgvector keeps as the typmod. The
 * skip index wants every array that long, so a column without one cannot be
 * indexed.
 */
static int
dimensions(Relation index, int attno) {
    Form_pg_attribute a = TupleDescAttr(index->rd_att, attno - 1);

    if (a->atttypmod < 1) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("a vector column of a chdb index needs a dimension"),
            errhint("Declare the column vector(n).")
        );
    }
    return a->atttypmod;
}

char*
chdb_search_vector_type(Relation index, int attno) {
    dimensions(index, attno);
    return pstrdup(CHDB_VECTOR_CH_TYPE);
}

/*
 * `vector_similarity('hnsw', '<function>', <dimensions>)`: the function is
 * the one the class's ORDER BY operator searches by, which the index must be
 * built with to serve it.
 */
char*
chdb_search_vector_index_type(Relation index, int attno) {
    Oid family     = index->rd_opfamily[attno - 1];
    CatCList* list = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(family));
    StrategyNumber strategy = InvalidStrategy;

    for (int i = 0; i < list->n_members && !strategy; i++) {
        Form_pg_amop op = (Form_pg_amop)GETSTRUCT(&list->members[i]->tuple);

        if (op->amoppurpose == AMOP_ORDER) {
            strategy = op->amopstrategy;
        }
    }
    ReleaseCatCacheList(list);
    if (!strategy) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
            errmsg(
                "chdb operator family %s has no ORDER BY operator to build a vector "
                "index with",
                get_opfamily_name(family, false)
            )
        );
    }
    return psprintf(
        CHDB_VECTOR_INDEX_TYPE,
        ask(index, attno, CHDB_VECTOR_PROC_DISTANCE_NAME, strategy),
        dimensions(index, attno)
    );
}

/* `<function>("<col>", [<q>])`, naming the function for the caller. */
static char*
distance_call(Relation index, const ChdbColumn* col, ScanKey orderby, const char** fn) {
    Oid argtype = OidIsValid(orderby->sk_subtype) ? orderby->sk_subtype : col->typid;
    StringInfoData buf;

    *fn = ask(
        index, orderby->sk_attno, CHDB_VECTOR_PROC_DISTANCE_NAME, orderby->sk_strategy
    );
    initStringInfo(&buf);
    appendStringInfo(&buf, "%s(%s, ", *fn, col->name);
    chdb_search_append_vector(&buf, orderby->sk_argument, argtype);
    appendStringInfoChar(&buf, ')');
    return buf.data;
}

/* The inner product is a similarity: larger is nearer. */
static bool
is_similarity(const char* fn) {
    return strcmp(fn, CHDB_VECTOR_FN_IP) == 0;
}

/*
 * The distance Postgres gets back for `col <op> q`: the function's value, or
 * for the inner product its negative, which is how pgvector defines <#>.
 */
char*
chdb_search_vector_distance(Relation index, const ChdbColumn* col, ScanKey orderby) {
    const char* fn;
    char* call = distance_call(index, col, orderby, &fn);

    return is_similarity(fn) ? psprintf("-%s", call) : call;
}

/*
 * ` ORDER BY <function>(col, q) [DESC] LIMIT <n> SETTINGS ...`, the shape
 * ClickHouse's vector search recognizes (useVectorSearch.cpp): one sort key
 * that is the distance function itself, ascending for a distance and
 * descending for the inner product, over a LIMIT no larger than
 * max_limit_for_vector_search_queries. An index scan has no LIMIT of its
 * own, so it asks for as many rows as the index serves, the most ClickHouse
 * would return through it; chdb_vector supplies the settings, as it owns
 * the GUCs behind them. Returns whether the order is descending.
 */
bool
chdb_search_vector_order(
    Relation index,
    StringInfo buf,
    const ChdbColumn* col,
    ScanKey orderby,
    int64 limit
) {
    const char* fn;
    char* call = distance_call(index, col, orderby, &fn);
    bool desc  = is_similarity(fn);

    appendStringInfo(buf, " ORDER BY %s%s", call, desc ? " DESC" : "");
    if (limit >= 0) {
        appendStringInfo(buf, " LIMIT " INT64_FORMAT, limit);
    } else {
        appendStringInfo(buf, " LIMIT getSetting('%s')", CHDB_VECTOR_SETTING_MAX_LIMIT);
    }
    appendStringInfo(
        buf,
        " %s",
        ask(index,
            orderby->sk_attno,
            CHDB_VECTOR_PROC_QUERY_SETTINGS,
            orderby->sk_strategy)
    );
    return desc;
}
