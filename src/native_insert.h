#ifndef CHDB_NATIVE_INSERT_H
#define CHDB_NATIVE_INSERT_H

/*
 * Rows on their way into a relation during a COPY from chDB (native_recv.c):
 * the multi-insert buffer, and the defaults of the columns the COPY leaves
 * out. copyfrom.c's CopyMultiInsertInfo, CopyMultiInsertBuffer and the
 * defaults BeginCopyFrom prepares, for one relation.
 */

#include "postgres.h"

#include "access/heapam.h"
#include "commands/trigger.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "utils/relcache.h"

/* Rows to buffer before a table_multi_insert, as copyfrom.c does. */
#define CHDB_MAX_BUFFERED_TUPLES 1000

/*
 * Rows on their way into the relation, buffered when the target allows it.
 * copyfrom.c splits this across CopyMultiInsertInfo and CopyMultiInsertBuffer,
 * one buffer per partition; a single relation needs one of each.
 */
typedef struct chdbNativeInsert {
    EState* estate;
    ResultRelInfo* target;
    TransitionCaptureState* transition;
    CommandId cid;
    int ti_options;
    BulkInsertState bistate;
    bool buffered; /* target takes table_multi_insert */
    int nused;
    size_t bytes;
    TupleTableSlot* slots[CHDB_MAX_BUFFERED_TUPLES];
} chdbNativeInsert;

/* Writes the buffered rows out, as CopyMultiInsertBufferFlush does. */
extern void
chdb_native_insert_flush(chdbNativeInsert* ins);

/* Slot to build the next buffered row in. */
extern TupleTableSlot*
chdb_native_insert_slot(chdbNativeInsert* ins);

/* Stores the row chdb_native_insert_slot handed out, flushing once full. */
extern void
chdb_native_insert_store(chdbNativeInsert* ins, TupleTableSlot* slot);

/*
 * One row into `rri`, the routed partition when there is one. False when an
 * FDW took the row and stored nothing, which counts as no row inserted.
 */
extern bool
chdb_native_insert_row(chdbNativeInsert* ins, ResultRelInfo* rri, TupleTableSlot* slot);

/*
 * Defaults for the columns a COPY column list leaves out, which BeginCopyFrom
 * prepares in copyfrom.c.
 */
typedef struct chdbNativeDefaults {
    int n;
    int* dest; /* attribute offsets the defaults fill */
    ExprState** exprs;
} chdbNativeDefaults;

/* The defaults of `rel`'s columns outside `attnums`, planned and initialized. */
extern chdbNativeDefaults
chdb_native_defaults_for(Relation rel, List* attnums);

/* Evaluates the defaults into `slot`, per row so a volatile one varies. */
extern void
chdb_native_defaults_fill(
    const chdbNativeDefaults* defaults,
    EState* estate,
    TupleTableSlot* slot
);

#endif /* CHDB_NATIVE_INSERT_H */
