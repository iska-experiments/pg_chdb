#ifndef CHDB_SEARCH_BUFFER_H
#define CHDB_SEARCH_BUFFER_H

/*
 * The per-transaction insert buffer behind aminsert, shared by buffer.c,
 * which fills it and runs the transaction callbacks, marks.c, which keeps
 * its savepoint bookkeeping, and staging.c, which sends its rows to the
 * worker. See buffer.c for how it works.
 */

#include "postgres.h"

#include "access/xact.h"

#include "search.h"

/* One index's rows buffered by the current transaction. */
typedef struct Pending {
    Oid indexoid;
    uint64 generation; /* of the metapage when the first row was buffered */
    ChdbRowWriter* rw;
    char* table;   /* idx_<oid>.t_<generation> */
    char* collist; /* (ctid, xmin, ...) for the INSERT */
    char* staging; /* <table>_tx_<xid>, set once rows have been staged */
    List* marks;   /* of Mark*, innermost last; private to marks.c */
    List* staged;  /* of Staged*, levels with rows staged; private to marks.c */
    /* Staged (sub)transaction ids since rolled back, as ints; see marks.c. */
    List* excluded;
    /* The subtransaction whose rebuild set these rows aside, else Invalid. */
    SubTransactionId superseded;
    bool poisoned; /* rows are lost: the buffer refuses to go on, COMMIT must error */
} Pending;

/* ---- marks.c ---- */
/* Takes the level's checkpoint before its first row, unless it has one. */
extern void
chdb_search_mark_level(Pending* p, SubTransactionId subid);
extern void
chdb_search_free_marks(Pending* p);
/* Rewinds or merges the level's rows as its savepoint ends, if it has a mark. */
extern void
chdb_search_settle_marks(
    Pending* p,
    SubXactEvent event,
    SubTransactionId mySubid,
    SubTransactionId parentSubid
);
/* The buffer's rows went to the staging table: the marks become staged levels. */
extern void
chdb_search_marks_shipped(Pending* p);
/* `1, 2`, the excluded ids for an `xmin NOT IN (...)`, or NULL when none. */
extern char*
chdb_search_excluded_xids(Pending* p);

/* ---- staging.c ---- */
/* Moves the transaction's buffered rows for the index into its staging table. */
extern void
chdb_search_stage_rows(Pending* p);
/* Sends everything buffered and staged into the index's table, at commit. */
extern void
chdb_search_flush_pending(Pending* p);
/* Drops the staging table now rather than at abort. */
extern void
chdb_search_drop_staging(Pending* p);
/* The same, warning rather than failing: for a commit that goes on without it. */
extern void
chdb_search_abandon_staging(Pending* p);

#endif /* CHDB_SEARCH_BUFFER_H */
