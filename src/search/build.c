/*
 * ambuild and ambuildempty: streams the heap into the new ClickHouse table in
 * 8 MiB Native blocks, bypassing the per-transaction buffer.
 */

#include "postgres.h"

#include "access/relation.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "pg-clickhouse-encode.h"

#include "search.h"

/* Matches src/native.c: ClickHouse coalesces small blocks itself. */
#define BLOCK_BYTES (8 * 1024 * 1024)

/* ---- ambuild ---- */

typedef struct BuildState {
    ChdbRowWriter* rw;
    chdbSearchConn* conn;
    double indtuples;
} BuildState;

static void
build_callback(
    Relation index,
    ItemPointer tid,
    Datum* values,
    bool* isnull,
    bool tupleIsAlive,
    void* state
) {
    BuildState* bs = state;

    /* Recently dead tuples stay: an older snapshot may still see them. */
    chdb_rowwriter_append(bs->rw, tid, InvalidTransactionId, values, isnull);
    bs->indtuples++;

    if (chdb_rowwriter_bytes(bs->rw) >= BLOCK_BYTES) {
        size_t len;
        void* block = chdb_rowwriter_take(bs->rw, &len);

        chdb_channel_write(chdb_search_channel(bs->conn), block, len);
        pfree(block);
    }
    CHECK_FOR_INTERRUPTS();
}

IndexBuildResult*
chdb_search_ambuild(Relation heap, Relation index, struct IndexInfo* indexInfo) {
    BuildState bs = {};
    double reltuples;

    if (RelationGetNumberOfBlocks(index) != 0) {
        elog(
            ERROR, "index \"%s\" already contains data", RelationGetRelationName(index)
        );
    }

    chdb_meta_init(index, MAIN_FORKNUM);

    /*
     * Registered before any DDL, so a build that fails halfway is undone too.
     * A new index loses its whole store on abort; a rebuild of an existing
     * one loses only the generation it was writing, as the metapage then goes
     * back to naming the previous table.
     */
    if (index->rd_createSubid != InvalidSubTransactionId) {
        chdb_search_drop_on_abort(RelationGetRelid(index));
    } else {
        chdb_search_drop_statement_on_abort(
            RelationGetRelid(index),
            psprintf("DROP TABLE IF EXISTS %s", chdb_search_table_name(index))
        );
    }
    chdb_search_create_store(index);

    bs.rw   = chdb_rowwriter_new(index);
    bs.conn = chdb_search_connect();

    PG_TRY();
    {
        char* sql = psprintf(
            "INSERT INTO %s %s",
            chdb_search_table_name(index),
            chdb_search_column_list(index)
        );

        chdb_search_log_sql("insert", sql);
        chdb_search_insert(bs.conn, RelationGetRelid(index), sql);
        reltuples = table_index_build_scan(
            heap, index, indexInfo, true, true, build_callback, &bs, NULL
        );

        if (chdb_rowwriter_rows(bs.rw)) {
            size_t len;
            void* block = chdb_rowwriter_take(bs.rw, &len);

            chdb_channel_write(chdb_search_channel(bs.conn), block, len);
            pfree(block);
        }
        chdb_search_finish(bs.conn);
    }
    PG_FINALLY();
    { chdb_search_close(bs.conn); }
    PG_END_TRY();

    chdb_rowwriter_free(bs.rw);
    chdb_meta_note_flush(index);

    IndexBuildResult* result = palloc(sizeof(IndexBuildResult));

    result->heap_tuples  = reltuples;
    result->index_tuples = bs.indtuples;
    return result;
}

void
chdb_search_ambuildempty(Relation index) {
    chdb_meta_init(index, INIT_FORKNUM);
}
