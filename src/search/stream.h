#ifndef CHDB_SEARCH_STREAM_H
#define CHDB_SEARCH_STREAM_H

/*
 * A SELECT streamed back from the worker and decoded row by row (scan.c),
 * for the index scan, the custom scan, the fail-safe check and VACUUM.
 * search.h has the rest of the access method's interface.
 */

#include "postgres.h"

#include "storage/itemptr.h"
#include "utils/rel.h"

#include "client.h"
#include "pg-clickhouse-decode.h"

/*
 * A running SELECT against the worker, each column decoded to a Postgres type:
 * for a scan `SELECT ctid[, distances][, scores]`, int8 (the packed TID),
 * then float8s, then float4s.
 */
typedef struct ChdbStream {
    chdbSearchConn* conn;
    pgch_reader reader;
    void** states;
    Datum* vals;
    bool* nulls;
    int ncols;
    MemoryContext cxt;
    MemoryContext rowcxt;
    bool done;
} ChdbStream;

extern ChdbStream*
chdb_search_stream_open(
    Oid indexoid,
    uint64 generation,
    const char* sql,
    int ndist,
    int nscores,
    MemoryContext cxt
);
extern ChdbStream*
chdb_search_stream_query(
    Oid indexoid,
    uint64 generation,
    const char* sql,
    const Oid* types,
    int ncols,
    MemoryContext cxt
);
/*
 * Next row into s->vals, valid until the next call; false at the end, which
 * also closes the stream. `tid`, if given, takes the packed ctid of column 0.
 */
extern bool
chdb_search_stream_next(ChdbStream* s, ItemPointer tid);
/* Abandons the stream, finishing it only if it ran to the end. */
extern void
chdb_search_stream_close(ChdbStream* s);
/*
 * Whether `tid` names a block the heap has, so that a store holding blocks
 * past the heap's end never makes the heap fetch raise; *nblocks caches the
 * heap's size between calls and starts at zero.
 */
extern bool
chdb_search_tid_in_heap(Relation heap, BlockNumber* nblocks, ItemPointer tid);

#endif /* CHDB_SEARCH_STREAM_H */
