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
 * The table's UUID is fixed by the index OID and the generation, and its
 * parts live on the index's callback object storage under a key prefix
 * naming the generation, so that the engine's own metadata is a cache: a
 * worker that starts without it attaches the table again with the same
 * statement (chdb_search_attach_sql) and the disk finds the parts where
 * the UUID puts them, which is what the index relation's pages hold.
 *
 * ctid encoding. The first column is `ctid UInt64`, the heap TID packed as
 * (block << 16) | offset, which is ORDER BY key and the join back to the heap.
 * Block numbers need 32 bits and offsets 16, so 48 bits are used and the
 * value sorts in physical order. `xmin UInt32` is the inserting transaction
 * id (truncated, so a diagnostic only); visibility is always decided by
 * fetching the heap tuple.
 *
 * Columns. columns.c names and types the indexed attributes and decides the
 * kind of each (text, text array, vector or plain) from its operator class.
 * Names are always quoted; types come from pgch_ch_type_for, so text is
 * Nullable(String) and text[] is Array(Nullable(String)); ClickHouse's text
 * index accepts both and NULLs survive. A NULL array is stored as the empty
 * array. A vector column of chdb_vector's classes is Array(Float32) under a
 * `TYPE vector_similarity('hnsw', <function>, <dimensions>)` index
 * (vector.c), which a search orders by.
 *
 * Example, for CREATE INDEX ON docs USING chdb (body text_ops
 * (tokenizer = 'ngrams', ngram_size = 3), tags text_array_ops, author columnar_ops):
 *
 *   CREATE DATABASE IF NOT EXISTS idx_16401
 *   CREATE TABLE idx_16401.t_7342 UUID '00004011-0000-0000-0000-1cae00000000'
 *     (ctid UInt64, xmin UInt32, "body" Nullable(String),
 *     "tags" Array(Nullable(String)), "author" Nullable(String),
 *     INDEX "body_idx" "body" TYPE text(tokenizer = ngrams(3),
 *       preprocessor = lowerUTF8("body")),
 *     INDEX "tags_idx" "tags" TYPE text(tokenizer = array,
 *       preprocessor = lowerUTF8("tags")))
 *     ENGINE = MergeTree ORDER BY ctid
 *     SETTINGS disk = disk(type = 'callback', storage_name = 'pg_16401',
 *       key_prefix = 'g7342'),
 *       enable_block_number_column = 1, enable_block_offset_column = 1
 *   ATTACH TABLE IF NOT EXISTS idx_16401.t_7342 UUID '...' (the same)
 *   CREATE TABLE idx_16401.t_7342_tx_912 UUID '...' (the same, key_prefix = 's7342_tx_912')
 *   INSERT INTO idx_16401.t_7342 (ctid, xmin, "body", "tags", "author")
 *   SELECT ctid FROM idx_16401.t_7342 WHERE hasAllTokens("body", 'running shoes')
 *   DELETE FROM idx_16401.t_7342 WHERE ctid IN (4294967296, ...)
 *     SETTINGS lightweight_delete_mode = 'lightweight_update_force'
 *   OPTIMIZE TABLE idx_16401.t_7342 FINAL
 *   DROP TABLE IF EXISTS idx_16401.t_7342 SYNC
 *
 * An INSERT carries no FORMAT clause: the worker streams it as Native. A
 * DROP TABLE says SYNC: ClickHouse's Atomic databases otherwise keep a
 * dropped table's parts for minutes, and a DROP DATABASE does not wait for
 * them, so blobs would outlive the index. Every statement is logged with
 * elog(DEBUG1) before it is sent.
 */

#include "postgres.h"

#include <string.h>

#include "pg-clickhouse.h"

#include "search.h"
#include "vector.h"

char*
chdb_search_table_of(Oid indexoid, uint64 generation) {
    return psprintf(CHDB_STORE_TABLE_FMT, indexoid, generation);
}

/* The table the index's metapage names. */
char*
chdb_search_table_name(Relation index) {
    return chdb_search_table_of(RelationGetRelid(index), chdb_meta_generation(index));
}

/* `ctid UInt64, xmin UInt32, name type, ...`, the Native block's schema. */
char*
chdb_search_structure(const ChdbColumn* cols, int natts) {
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfoString(&buf, "ctid UInt64, xmin UInt32");
    for (int i = 0; i < natts; i++) {
        appendStringInfo(&buf, ", %s %s", cols[i].name, cols[i].type);
    }
    return buf.data;
}

/* `idx_<oid>.t_<generation>_tx_<fxid>`, a transaction's staging table. */
char*
chdb_search_staging_of(Oid indexoid, uint64 generation, uint64 fxid) {
    return psprintf(CHDB_STORE_TABLE_FMT "_tx_" UINT64_FORMAT, indexoid, generation, fxid);
}

/*
 * A table's UUID, from the index OID, the generation and, for a staging
 * table, the transaction: 32 hex digits in the 8-4-4-4-12 groups. ClickHouse
 * checks no version bits.
 */
static char*
table_uuid(Oid indexoid, uint64 generation, uint64 fxid) {
    return psprintf(
        "%08x-%04x-%04x-%04x-%04x%08x",
        indexoid,
        (unsigned)(generation >> 48),
        (unsigned)((generation >> 32) & 0xffff),
        (unsigned)((generation >> 16) & 0xffff),
        (unsigned)(generation & 0xffff),
        (unsigned)(fxid & 0xffffffff)
    );
}

/*
 * `verb` (CREATE TABLE, or ATTACH TABLE IF NOT EXISTS) and the table's whole
 * definition: the build's table for a zero `fxid`, else that transaction's
 * staging table, which differs in name, UUID and key prefix alone.
 */
static char*
table_sql(Relation index, const char* verb, uint64 fxid) {
    ChdbColumn* cols  = chdb_search_columns(index);
    Oid oid           = RelationGetRelid(index);
    uint64 generation = chdb_meta_generation(index);
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfo(
        &buf,
        "%s %s UUID '%s' (ctid UInt64, xmin UInt32",
        verb,
        fxid ? chdb_search_staging_of(oid, generation, fxid)
             : chdb_search_table_of(oid, generation),
        table_uuid(oid, generation, fxid)
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
            ", INDEX %s %s TYPE %s",
            idxname,
            cols[i].name,
            cols[i].kind == CHDB_COL_VECTOR
                ? chdb_search_vector_index_type(index, i + 1)
                : psprintf("text(%s)", chdb_search_skip_index_args(&cols[i]))
        );
    }
    /*
     * The parts live on the index's callback object storage, whose blobs
     * the worker holds (pagestore/) in the index relation's pages, written
     * ahead of the COMMIT that follows a flush in the WAL, so that the rows
     * a flush sends are as safe as Postgres's own data. The block columns
     * let VACUUM's DELETE patch parts in place (vacuum.c) instead of
     * rewriting them with a mutation, which the disk does not allow; a
     * store from before they were set takes them with ALTER TABLE ...
     * MODIFY SETTING, no REINDEX. One SETTINGS clause: ClickHouse rejects a
     * second.
     */
    appendStringInfo(
        &buf,
        ") ENGINE = MergeTree ORDER BY ctid SETTINGS " CHDB_STORE_DISK_FMT
        ", enable_block_number_column = 1, enable_block_offset_column = 1",
        oid,
        fxid ? psprintf(CHDB_STORE_STAGING_PREFIX_FMT, generation, fxid)
             : psprintf(CHDB_STORE_KEY_PREFIX_FMT, generation)
    );
    if (chdb_search_wants_phrase_search(cols, index->rd_att->natts)) {
        /* ClickHouse gates the index argument behind a MergeTree setting. */
        appendStringInfoString(
            &buf, ", allow_experimental_text_index_phrase_search = 1"
        );
    }
    return buf.data;
}

char*
chdb_search_create_sql(Relation index) {
    return table_sql(index, "CREATE TABLE", 0);
}

char*
chdb_search_staging_sql(Relation index, uint64 fxid) {
    return table_sql(index, "CREATE TABLE", fxid);
}

/*
 * The statements that put the index's current table, or a transaction's
 * staging table, back into an engine whose metadata does not have it: the
 * parts are found by the disk under the key prefix, and a table the engine
 * has is left alone.
 */
char*
chdb_search_attach_sql(Relation index, uint64 fxid) {
    return table_sql(index, "ATTACH TABLE IF NOT EXISTS", fxid);
}

/* `(ctid, xmin, a, b)`, the columns a Native INSERT names, in block order. */
char*
chdb_search_column_list(const ChdbColumn* cols, int natts) {
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfoString(&buf, "(ctid, xmin");
    for (int i = 0; i < natts; i++) {
        appendStringInfo(&buf, ", %s", cols[i].name);
    }
    appendStringInfoChar(&buf, ')');
    return buf.data;
}

void
chdb_search_run(Oid indexoid, uint64 generation, const char* sql) {
    chdbSearchConn* conn = chdb_search_connect();

    PG_TRY();
    {
        chdb_search_log_sql("exec", sql);
        chdb_search_exec(conn, indexoid, generation, sql);
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
chdb_search_try_run(Oid indexoid, uint64 generation, const char* sql) {
    PG_TRY();
    { chdb_search_run(indexoid, generation, sql); }
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

    /* No generation: the table these make does not exist yet. */
    chdb_search_run(oid, 0, psprintf("CREATE DATABASE IF NOT EXISTS idx_%u", oid));
    chdb_search_run(oid, 0, create);
}
