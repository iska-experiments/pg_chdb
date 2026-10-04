/*
 * ClickHouse DDL for a chdb index.
 *
 * Each index owns one ClickHouse database `idx_<indexrelid>` holding one
 * MergeTree table `t`. Statements name it in full, `idx_<oid>.t`, so they work
 * whichever default database the worker's session has.
 *
 * ctid encoding. The first column is `ctid UInt64`, the heap TID packed as
 * (block << 16) | offset, which is ORDER BY key and the join back to the heap.
 * Block numbers need 32 bits and offsets 16, so 48 bits are used and the
 * value sorts in physical order. `xmin UInt32` is the inserting transaction
 * id (truncated, so a diagnostic only); visibility is always decided by
 * fetching the heap tuple.
 *
 * Column naming. Every indexed attribute becomes a column named after the
 * index attribute (the heap column for plain columns), quoted with
 * pgch_quote_ch_ident so any Postgres name works. `ctid` and `xmin` are
 * reserved, and duplicate names (two expression columns, say) are rejected
 * because the table would be unusable.
 *
 * Types. text_ops columns are `String` and text_array_ops `Array(String)`;
 * a NULL is stored as '' or [], as the text index cannot hold NULL. Columnar
 * columns use pgch_ch_type_for, so NULLs survive as Nullable(...).
 *
 * Example, for CREATE INDEX ON docs USING chdb (body text_ops
 * (tokenizer = 'ngrams', ngram_size = 3), tags text_array_ops, author columnar_ops):
 *
 *   CREATE DATABASE IF NOT EXISTS idx_16401
 *   CREATE TABLE idx_16401.t (ctid UInt64, xmin UInt32, body String,
 *     tags Array(String), author Nullable(String),
 *     INDEX body_idx body TYPE text(tokenizer = ngrams(3), preprocessor =
 * lowerUTF8(body)), INDEX tags_idx tags TYPE text(tokenizer = array)) ENGINE =
 * MergeTree ORDER BY ctid INSERT INTO idx_16401.t (ctid, xmin, body, tags, author)
 * FORMAT Native SELECT ctid FROM idx_16401.t WHERE hasAllTokens(body, 'running shoes')
 *   DELETE FROM idx_16401.t WHERE ctid IN (4294967296, ...)
 *   OPTIMIZE TABLE idx_16401.t FINAL
 *
 * Every statement is logged with elog(DEBUG1) before it is sent.
 */

#include "postgres.h"

#include <string.h>

#include "catalog/pg_attribute.h"
#include "utils/lsyscache.h"

#include "pg-clickhouse.h"

#include "search.h"

char*
chdb_search_table_name(Oid indexoid) {
    return psprintf("idx_%u.t", indexoid);
}

static ChdbColumnKind
kind_of(Relation index, int i) {
    char* fam = get_opfamily_name(index->rd_opfamily[i], false);

    if (strcmp(fam, "text_ops") == 0) {
        return CHDB_COL_TEXT;
    }
    if (strcmp(fam, "text_array_ops") == 0) {
        return CHDB_COL_TEXT_ARRAY;
    }
    return CHDB_COL_COLUMNAR;
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

        cols[i].name  = pgch_quote_ch_ident(name);
        cols[i].kind  = kind_of(index, i);
        cols[i].typid = a->atttypid;
        switch (cols[i].kind) {
        case CHDB_COL_TEXT:
            cols[i].type = "String";
            break;
        case CHDB_COL_TEXT_ARRAY:
            cols[i].type = "Array(String)";
            break;
        default:
            cols[i].type =
                pgch_ch_type_for(a->atttypid, a->atttypmod, a->attnotnull, NULL);
        }
    }
    return cols;
}

/* `ctid UInt64, xmin UInt32, name type, ...`, the Native block's schema. */
char*
chdb_search_structure(Relation index) {
    ChdbColumn* cols = chdb_search_columns(index);
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfoString(&buf, "ctid UInt64, xmin UInt32");
    for (int i = 0; i < index->rd_att->natts; i++) {
        appendStringInfo(&buf, ", %s %s", cols[i].name, cols[i].type);
    }
    return buf.data;
}

char*
chdb_search_create_sql(Relation index) {
    ChdbColumn* cols = chdb_search_columns(index);
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfo(
        &buf,
        "CREATE TABLE %s (ctid UInt64, xmin UInt32",
        chdb_search_table_name(RelationGetRelid(index))
    );
    for (int i = 0; i < index->rd_att->natts; i++) {
        appendStringInfo(&buf, ", %s %s", cols[i].name, cols[i].type);
    }
    for (int i = 0; i < index->rd_att->natts; i++) {
        if (cols[i].kind == CHDB_COL_COLUMNAR) {
            continue;
        }

        /* The index name is a column name plus a suffix, quoted as a whole. */
        char* bare =
            psprintf("%s_idx", NameStr(TupleDescAttr(index->rd_att, i)->attname));
        char* idxname = pgch_quote_ch_ident(bare);

        appendStringInfo(
            &buf,
            ", INDEX %s %s TYPE text(%s)",
            idxname,
            cols[i].name,
            chdb_search_skip_index_args(index, i + 1, cols[i].name, cols[i].kind)
        );
    }
    appendStringInfoString(&buf, ") ENGINE = MergeTree ORDER BY ctid");
    return buf.data;
}

/* `(ctid, xmin, a, b)`, the columns a Native INSERT names, in block order. */
char*
chdb_search_column_list(Relation index) {
    ChdbColumn* cols = chdb_search_columns(index);
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfoString(&buf, "(ctid, xmin");
    for (int i = 0; i < index->rd_att->natts; i++) {
        appendStringInfo(&buf, ", %s", cols[i].name);
    }
    appendStringInfoChar(&buf, ')');
    return buf.data;
}

/* Runs one statement in the index's database on its own connection. */
void
chdb_search_run(Oid indexoid, const char* sql) {
    chdbSearchConn* conn = chdb_search_connect();

    PG_TRY();
    {
        chdb_search_log_sql("exec", sql);
        chdb_search_exec(conn, indexoid, sql);
    }
    PG_FINALLY();
    { chdb_search_close(conn); }
    PG_END_TRY();
}

/*
 * Creates the store for a fresh index. A rebuild (REINDEX, TRUNCATE) comes
 * through here too, so any table left from before is dropped first.
 */
void
chdb_search_create_store(Relation index) {
    Oid oid      = RelationGetRelid(index);
    char* tbl    = chdb_search_table_name(oid);
    char* create = chdb_search_create_sql(index); /* validates the columns first */
    char* db     = psprintf("idx_%u", oid);

    chdb_search_run(oid, psprintf("CREATE DATABASE IF NOT EXISTS %s", db));
    chdb_search_run(oid, psprintf("DROP TABLE IF EXISTS %s", tbl));
    chdb_search_run(oid, create);
}
