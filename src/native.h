#ifndef CHDB_NATIVE_H
#define CHDB_NATIVE_H

#include "postgres.h"

#include "funcapi.h"
#include "nodes/pg_list.h"
#include "utils/relcache.h"

#include "pg-clickhouse-decode.h"

#include "channel.h"

/*
 * A block source over the Native blocks `helper` streams, for pgch_reader_init,
 * allocated in the current memory context, which decoded blocks live in.
 */
extern pgch_block_source
chdb_native_source(chdbChannel* helper);

/* Source column name and ClickHouse type returned by DESCRIBE */
typedef struct chdbDescribedColumn {
    char* name;
    char* type;
} chdbDescribedColumn;

/*
 * Read DESCRIBE rows from `helper`
 * Return chdbDescribedColumn list in caller memory context
 */
extern List*
chdb_native_describe(chdbChannel* helper);

/*
 * Scans the `attnums` columns of `rel` into `helper` as Native blocks carrying
 * the columns `structure` declares, which is what ClickHouse matches them
 * against. Returns the rows sent.
 */
extern uint64_t
chdb_copy_send(Relation rel, const char* structure, List* attnums, chdbChannel* helper);

/*
 * Inserts the rows `helper` streams as Native blocks into the `attnums` columns
 * of `rel`, other columns taking their default. Runs under `rtable`, the range
 * table the caller has already checked. Returns the rows inserted, not counting
 * any a BEFORE trigger suppressed.
 */
extern uint64_t
chdb_copy_receive(
    Relation rel,
    List* attnums,
    List* rtable,
    List* rteperminfos,
    uint16_t encoding_check,
    chdbChannel* helper
);

/*
 * Execute a query against a temporary chDB database and return its rows,
 * mapping chDB values to the Postgres types named in the caller's column
 * definition list and streaming the results back to the client.
 */
Datum
chdb_select_receive(
    char* query,
    ReturnSetInfo* rsinfo,
    TupleDesc tupdesc,
    chdbChannel* helper
);

#endif /* CHDB_NATIVE_H */
