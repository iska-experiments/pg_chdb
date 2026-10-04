/*
 * ClickHouse DDL for a chdb index.
 *
 * Each index owns one ClickHouse database `idx_<indexrelid>` holding one
 * MergeTree table per build, `t_<generation>`, named after the metapage's
 * random generation. Statements name it in full, `idx_<oid>.t_<generation>`,
 * so they work whichever default database the worker's session has.
 *
 * Every rebuild (REINDEX, TRUNCATE, CLUSTER, a table rewrite) writes a new
 * generation beside the old one and touches nothing else: if it commits, the
 * metapage names the new table; if it rolls back, the old one. Whichever
 * lost is swept by the next VACUUM, since the build cannot read the previous
 * metapage once REINDEX has given the index a new relfilenode.
 *
 * ctid encoding. The first column is `ctid UInt64`, the heap TID packed as
 * (block << 16) | offset, which is ORDER BY key and the join back to the heap.
 * Block numbers need 32 bits and offsets 16, so 48 bits are used and the
 * value sorts in physical order. `xmin UInt32` is the inserting transaction
 * id (truncated, so a diagnostic only); visibility is always decided by
 * fetching the heap tuple.
 *
 * Columns. columns.c names and types the indexed attributes and decides the
 * kind of each (text, text array or plain) from its operator class. Names
 * are always quoted; types come from pgch_ch_type_for, so text is
 * Nullable(String) and text[] is Array(Nullable(String)); ClickHouse's text
 * index accepts both and NULLs survive. A NULL array is stored as the empty
 * array.
 *
 * Example, for CREATE INDEX ON docs USING chdb (body text_ops
 * (tokenizer = 'ngrams', ngram_size = 3), tags text_array_ops, author columnar_ops):
 *
 *   CREATE DATABASE IF NOT EXISTS idx_16401
 *   CREATE TABLE idx_16401.t_7342 (ctid UInt64, xmin UInt32, "body" Nullable(String),
 *     "tags" Array(Nullable(String)), "author" Nullable(String),
 *     INDEX "body_idx" "body" TYPE text(tokenizer = ngrams(3),
 *       preprocessor = lowerUTF8("body")),
 *     INDEX "tags_idx" "tags" TYPE text(tokenizer = array,
 *       preprocessor = lowerUTF8("tags")))
 *     ENGINE = MergeTree ORDER BY ctid
 *   INSERT INTO idx_16401.t_7342 (ctid, xmin, "body", "tags", "author")
 *   SELECT ctid FROM idx_16401.t_7342 WHERE hasAllTokens("body", 'running shoes')
 *   DELETE FROM idx_16401.t_7342 WHERE ctid IN (4294967296, ...)
 *   OPTIMIZE TABLE idx_16401.t_7342 FINAL
 *
 * An INSERT carries no FORMAT clause: the worker streams it as Native. Every
 * statement is logged with elog(DEBUG1) before it is sent.
 */

#include "postgres.h"

#include <string.h>

#include "pg-clickhouse.h"

#include "search.h"

/* `idx_<oid>.t_<generation>`: the table the index's metapage names. */
char*
chdb_search_table_name(Relation index) {
    return psprintf(
        "idx_%u.t_" UINT64_FORMAT, RelationGetRelid(index), chdb_meta_generation(index)
    );
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
        &buf, "CREATE TABLE %s (ctid UInt64, xmin UInt32", chdb_search_table_name(index)
    );
    for (int i = 0; i < index->rd_att->natts; i++) {
        appendStringInfo(&buf, ", %s %s", cols[i].name, cols[i].type);
    }
    for (int i = 0; i < index->rd_att->natts; i++) {
        if (cols[i].kind == CHDB_COL_COLUMNAR) {
            continue;
        }

        /* The index name is a column name plus a suffix, quoted as a whole. */
        char* idxname = chdb_search_quote_ident(
            psprintf("%s_idx", NameStr(TupleDescAttr(index->rd_att, i)->attname))
        );

        appendStringInfo(
            &buf,
            ", INDEX %s %s TYPE text(%s)",
            idxname,
            cols[i].name,
            chdb_search_skip_index_args(index, i + 1, cols[i].name, cols[i].kind)
        );
    }
    appendStringInfoString(&buf, ") ENGINE = MergeTree ORDER BY ctid");
    if (chdb_search_wants_phrase_search(index, cols)) {
        /* ClickHouse gates the index argument behind a MergeTree setting. */
        appendStringInfoString(
            &buf, " SETTINGS allow_experimental_text_index_phrase_search = 1"
        );
    }
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
        chdb_search_exec(conn, indexoid, CHDB_SEARCH_NO_GENERATION, sql);
    }
    PG_FINALLY();
    { chdb_search_close(conn); }
    PG_END_TRY();
}

/*
 * Reports the error being handled as a WARNING, for cleanup past the point
 * where failing would help: after the commit, or inside VACUUM's sweep.
 */
void
chdb_search_warn_failure(Oid indexoid) {
    ErrorData* e = CopyErrorData();

    FlushErrorState();
    ereport(
        WARNING,
        errmsg("chdb_search: could not clean up index %u: %s", indexoid, e->message)
    );
    FreeErrorData(e);
}

/* chdb_search_run, warning instead of raising when the statement fails. */
void
chdb_search_try_run(Oid indexoid, const char* sql) {
    PG_TRY();
    { chdb_search_run(indexoid, sql); }
    PG_CATCH();
    { chdb_search_warn_failure(indexoid); }
    PG_END_TRY();
}

/*
 * Creates the table of the index's current generation. A rebuild comes
 * through here too and leaves the previous generation's table alone, for
 * the transaction may still roll back to it.
 */
void
chdb_search_create_store(Relation index) {
    Oid oid      = RelationGetRelid(index);
    char* create = chdb_search_create_sql(index); /* validates the columns first */

    chdb_search_run(oid, psprintf("CREATE DATABASE IF NOT EXISTS idx_%u", oid));
    chdb_search_run(oid, create);
}
