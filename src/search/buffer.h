#ifndef CHDB_SEARCH_BUFFER_H
#define CHDB_SEARCH_BUFFER_H

/*
 * The per-transaction insert buffer behind aminsert, shared by buffer.c,
 * which fills it and runs the transaction callbacks, and staging.c, which
 * sends its rows to the worker. See buffer.c for how it works.
 */

#include "postgres.h"

#include "access/xact.h"

#include "search.h"

/* One index's rows buffered by the current transaction. */
typedef struct Pending {
    Oid indexoid;
    ChdbRowWriter* rw;
    char* table;   /* idx_<oid>.t_<generation> */
    char* collist; /* (ctid, xmin, ...) for the INSERT */
    char* staging; /* <table>_tx_<xid>, set once rows have been staged */
    List* marks;   /* of Mark*, innermost last; private to buffer.c */
    /* The subtransaction whose rebuild set these rows aside, else Invalid. */
    SubTransactionId superseded;
} Pending;

/* ---- staging.c ---- */
/* Moves a top-level transaction's rows into its staging table. */
extern void
chdb_search_stage_rows(Pending* p);
/* Sends everything buffered and staged into the index's table, at commit. */
extern void
chdb_search_flush_pending(Pending* p);
/* Drops the staging table now rather than at abort. */
extern void
chdb_search_drop_staging(Pending* p);

#endif /* CHDB_SEARCH_BUFFER_H */
