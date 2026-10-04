#ifndef CHDB_SEARCH_STREAM_H
#define CHDB_SEARCH_STREAM_H

/*
 * A SELECT streamed back from the worker and decoded row by row (stream.c),
 * for the index scan, the custom scan, the aggregate scan, the score's
 * counts and VACUUM. search.h has the rest of the access method's
 * interface.
 */

#include "postgres.h"

#include "storage/itemptr.h"
#include "utils/rel.h"

#include "client.h"
#include "pg-clickhouse-decode.h"

/*
 * A running SELECT against the worker, each column decoded to a Postgres type:
 * for a scan `SELECT ctid, xmin[, distances][, scores]`, int8 (the packed
 * TID), int8 (the inserting transaction id), then float8s, then float4s.
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
    /* For chdb_search_stream_fetchable: the heap's size as last measured, */
    BlockNumber heap_nblocks;
    /* and the last transaction judged, with the verdict. */
    TransactionId last_xmin;
    bool last_aborted;
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
 * also closes the stream. `tid`, if given, takes the packed ctid of column 0,
 * and `xmin` the transaction id of column 1.
 */
extern bool
chdb_search_stream_next(ChdbStream* s, ItemPointer tid, TransactionId* xmin);
/* Abandons the stream, finishing it only if it ran to the end. */
extern void
chdb_search_stream_close(ChdbStream* s);
/*
 * For a scan's stream, whether the heap may be asked for the tuple of the
 * row just read, `tid`: it names a block the heap has, so that a store
 * holding blocks past the heap's end never makes the heap fetch raise, and
 * its transaction did not end without committing (xmin.c), as the heap may
 * have given that TID to another row since.
 */
extern bool
chdb_search_stream_fetchable(ChdbStream* s, Relation heap, ItemPointer tid);

#endif /* CHDB_SEARCH_STREAM_H */
