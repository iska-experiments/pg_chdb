/*
 * The columns of a chdb index as its ClickHouse table has them: the name,
 * always quoted, the type from pgch_ch_type_for, and the kind, which the
 * column's operator class decides and which says whether the column gets a
 * text skip index. The checks a column must pass live here too; ddl.c
 * builds the statements from the result.
 */

#include "postgres.h"

#include <string.h>

#include "access/htup_details.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"

#include "pg-clickhouse.h"

#include "search.h"

/*
 * `"<name>"`, whatever the name: pgch_quote_ch_ident leaves a plain word
 * bare, and ClickHouse then reads index, constraint or projection as the
 * keyword and inf or nan as a Float64 literal, so `WHERE inf = 1` found
 * nothing and `inf > 0` every row, with no error and no recheck.
 */
char*
chdb_search_quote_ident(const char* name) {
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfoChar(&buf, '"');
    for (const char* p = name; *p; p++) {
        if (*p == '"' || *p == '\\') {
            appendStringInfoChar(&buf, *p == '"' ? '"' : '\\');
        }
        appendStringInfoChar(&buf, *p);
    }
    appendStringInfoChar(&buf, '"');
    return buf.data;
}

/*
 * A class's options support function (number 1) says how its columns are
 * stored: ours resolve to the C functions below, which are compared by
 * address, so neither a family's name nor the extension's schema matters,
 * and a class of another extension, or one without the proc, is columnar.
 */
ChdbColumnKind
chdb_search_proc_kind(Oid proc) {
    FmgrInfo finfo;

    if (!OidIsValid(proc)) {
        return CHDB_COL_COLUMNAR;
    }
    fmgr_info(proc, &finfo);
    if (finfo.fn_addr == chdb_search_text_options) {
        return CHDB_COL_TEXT;
    }
    if (finfo.fn_addr == chdb_search_text_array_options) {
        return CHDB_COL_TEXT_ARRAY;
    }
    return CHDB_COL_COLUMNAR;
}

static ChdbColumnKind
kind_of(Relation index, int i) {
    return chdb_search_proc_kind(index_getprocid(index, i + 1, 1));
}

/*
 * ClickHouse compares String columns bytewise while Postgres orders text by
 * its collation, and the scan does not recheck: under en_US `author >= 'a'`
 * finds 'ann' and 'Bob' by seqscan but only 'ann' through the index, and a
 * nondeterministic collation even breaks `=`. So a filterable text column
 * needs a bytewise collation, as btree's text_pattern_ops does.
 */
static void
check_text_collation(Relation index, int i, Oid typid) {
    bool bytewise;

    if (typid != TEXTOID && typid != VARCHAROID && typid != BPCHAROID &&
        typid != NAMEOID) {
        return;
    }
#if PG_VERSION_NUM >= 180000
    bytewise = pg_newlocale_from_collation(index->rd_indcollation[i])->collate_is_c;
#else
    bytewise = lc_collate_is_c(index->rd_indcollation[i]);
#endif
    if (!bytewise) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg(
                "chdb columnar_ops on a text column requires a C or POSIX collation"
            ),
            errhint("Declare the column COLLATE \"C\" or index (col COLLATE \"C\").")
        );
    }
}

/*
 * A plain column serves queries through the operators its family has for
 * its type, so a type with none (int[], jsonb, an enum), which columnar_ops
 * admits as the default class for anyelement, would be stored at every
 * write and never searched. Binary-coercible types count, as varchar's
 * filters are text's, and an ordering operator is as good as a comparison.
 */
static void
check_operators(Relation index, int i, Oid typid) {
    Oid family     = index->rd_opfamily[i];
    CatCList* list = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(family));
    bool found     = false;

    for (int j = 0; j < list->n_members && !found; j++) {
        Form_pg_amop op = (Form_pg_amop)GETSTRUCT(&list->members[j]->tuple);

        found = op->amoplefttype == typid || IsBinaryCoercible(typid, op->amoplefttype);
    }
    ReleaseCatCacheList(list);
    if (!found) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg(
                "type %s has no operators in chdb operator family %s",
                format_type_be(typid),
                get_opfamily_name(family, false)
            ),
            errhint(
                "Index text with text_ops and text[] with text_array_ops; a stored "
                "column needs comparison operators for its type in the family."
            )
        );
    }
}

ChdbColumn*
chdb_search_columns(Relation index) {
    int natts        = index->rd_att->natts;
    ChdbColumn* cols = palloc0(sizeof(ChdbColumn) * natts);

    for (int i = 0; i < natts; i++) {
        Form_pg_attribute a = TupleDescAttr(index->rd_att, i);
        const char* name    = NameStr(a->attname);

        if (strcmp(name, "ctid") == 0 || strcmp(name, "xmin") == 0) {
            ereport(
                ERROR,
                errcode(ERRCODE_RESERVED_NAME),
                errmsg("column name \"%s\" is reserved by chdb indexes", name),
                errdetail("The ClickHouse table uses \"ctid\" and \"xmin\" itself.")
            );
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(NameStr(TupleDescAttr(index->rd_att, j)->attname), name) == 0) {
                ereport(
                    ERROR,
                    errcode(ERRCODE_DUPLICATE_COLUMN),
                    errmsg("chdb index has two columns named \"%s\"", name),
                    errhint(
                        "Give expression columns distinct names with a view or "
                        "generated column."
                    )
                );
            }
        }

        cols[i].name = chdb_search_quote_ident(name);
        cols[i].kind = kind_of(index, i);
        if (cols[i].kind == CHDB_COL_COLUMNAR) {
            check_operators(index, i, a->atttypid);
            check_text_collation(index, i, a->atttypid);
        }
        cols[i].typid = a->atttypid;
        cols[i].type = pgch_ch_type_for(a->atttypid, a->atttypmod, a->attnotnull, NULL);
    }
    return cols;
}
