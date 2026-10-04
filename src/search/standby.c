/*
 * The worker on a server in recovery, and its promotion. A standby has the
 * index pages, replayed from the primary's WAL, and serves searches from
 * them: the engine runs read-only (engine/readonly.c), its tables are
 * attached read-only and put back whenever the pages change under them
 * (attach.c), and the page store writes nothing (pagestore/). Recovery
 * ends when the server is promoted, which the worker notices from its
 * event loop: the engine stops, its directory, a cache of tables attached
 * read-only, is emptied, and the relations and tables are forgotten, so
 * the next request starts a read-write engine and attaches afresh,
 * recovering what a crashed primary left dirty. Nothing is rebuilt: the
 * pages are the store. See standby.h.
 */

#include "postgres.h"

#include "access/xlog.h"
#include "miscadmin.h"

#include "attach.h"
#include "engine_proc.h"
#include "pagestore/pagestore.h"
#include "standby.h"
#include "sweep.h"

/* Milliseconds between looks at the recovery state while it lasts. */
#define CHDB_SEARCH_PROMOTION_POLL_MS 1000

static bool in_recovery;

void
chdb_search_standby_init(void) {
    in_recovery = RecoveryInProgress();
    if (in_recovery) {
        ereport(
            LOG,
            errmsg(
                "chdb_search: worker for database %u serves its indexes read-only "
                "while the server is in recovery",
                MyDatabaseId
            )
        );
    }
}

long
chdb_search_standby_timeout(void) {
    return in_recovery ? CHDB_SEARCH_PROMOTION_POLL_MS : -1;
}

void
chdb_search_standby_poll(void) {
    if (!in_recovery || RecoveryInProgress()) {
        return;
    }
    in_recovery = false;
    engine_stop();
    chdb_search_empty_engine_dir(MyDatabaseId);
    chdb_search_attach_reset();
    chdb_pagestore_reset();
    ereport(
        LOG,
        errmsg(
            "chdb_search: worker for database %u promoted, the engine restarts read-write",
            MyDatabaseId
        )
    );
}
