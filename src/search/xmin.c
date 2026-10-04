/*
 * The rows of transactions that never committed. A flush sends a
 * transaction's rows to the store before the commit record is written, so
 * a crash in between, or a commit that fails after its flush, leaves rows
 * in the store whose heap tuples are dead; and a dead tuple's TID is reused
 * once pruned, so a scan that returned the row would fetch another row's
 * tuple. Every store row carries the transaction id that inserted it, and
 * a scan skips, as VACUUM deletes, the rows whose transaction is known to
 * have ended without committing. A build's rows carry no id: they are the
 * heap as the build saw it, and the build's own commit or rollback decides
 * the whole relation.
 */

#include "postgres.h"

#include "access/transam.h"
#include "storage/procarray.h"

#include "search.h"

bool
chdb_search_xmin_aborted(Relation heap, TransactionId xmin) {
    TransactionId frozen = heap->rd_rel->relfrozenxid;

    if (!TransactionIdIsNormal(xmin)) {
        return false;
    }
    /*
     * The commit log answers for the ids between the heap's frozen horizon
     * and the next id to be assigned; older ones are a frozen heap's, long
     * decided and their losers removed, and an id that wrapped past the
     * horizon reads as from the future. Both count as committed.
     */
    if (!TransactionIdIsNormal(frozen) || TransactionIdPrecedes(xmin, frozen) ||
        !TransactionIdPrecedes(xmin, ReadNextTransactionId())) {
        return false;
    }
    if (TransactionIdDidCommit(xmin)) {
        return false;
    }
    /* Not committed: running still, which the heap fetch judges, or over. */
    return !TransactionIdIsInProgress(xmin);
}
