/*
 * The chdb_search background worker's lifecycle: one per database, claiming
 * its registry slot, sweeping what drops left behind, opening its socket,
 * serving until told to stop. The chDB store is held by the
 * chdb_search_engine child it supervises. See protocol.h
 * for the framing and dev/design/chdb_search.md for why there is a worker at
 * all.
 */

#include "postgres.h"

#include <signal.h>

#include "access/xact.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"

#include "engine_proc.h"
#include "pagestore/pagestore.h"
#include "registry.h"
#include "serve.h"
#include "standby.h"
#include "sweep.h"
#include "worker.h"

/* PG 19 types pqsignal's handlers and names SIG_DFL in that type. */
#ifndef PG_SIG_DFL
#define PG_SIG_DFL SIG_DFL
#endif

static Oid worker_dboid;
static bool slot_claimed;

static void
worker_exit(int code pg_attribute_unused(), Datum arg pg_attribute_unused()) {
    chdb_search_unlisten();
    engine_stop();
    if (slot_claimed) {
        chdb_search_free_slot(worker_dboid, MyProcPid);
    }
}

void
chdb_search_worker_main(Datum arg) {
    worker_dboid = DatumGetObjectId(arg);

    pqsignal(SIGHUP, SignalHandlerForConfigReload);
    pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
    /* The engine is our child, and its death is read with waitpid. */
    pqsignal(SIGCHLD, PG_SIG_DFL);
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnectionByOid(worker_dboid, InvalidOid, 0);

    /* Before shared memory goes, since it frees the registry slot. */
    before_shmem_exit(worker_exit, (Datum)0);

    workerRegistry* reg = chdb_search_registry();
    bool taken          = false;

    SpinLockAcquire(&reg->lock);
    workerSlot* slot = chdb_search_find_slot(reg, worker_dboid);
    if (slot && !(slot->state == WORKER_RUNNING && slot->pid != MyProcPid &&
                  chdb_search_pid_alive(slot->pid))) {
        slot->dboid = worker_dboid;
        slot->state = WORKER_RUNNING;
        slot->pid   = MyProcPid;
        taken       = true;
    }
    SpinLockRelease(&reg->lock);
    if (!taken) {
        /* Postmaster restarted a worker whose replacement is already up. */
        proc_exit(0);
    }
    slot_claimed = true;

    /* The engine the sweep starts asks for its blobs at once. */
    chdb_pagestore_init();

    /* The engine's stale cache and what drops left behind go before anything is served. */
    PG_TRY();
    { chdb_search_sweep(worker_dboid); }
    PG_CATCH();
    {
        if (IsTransactionState()) {
            AbortCurrentTransaction();
        }
        EmitErrorReport();
        FlushErrorState();
    }
    PG_END_TRY();

    chdb_search_standby_init();
    chdb_search_listen(worker_dboid);
    ereport(LOG, errmsg("chdb_search: worker for database %u listening", worker_dboid));

    chdb_search_serve();

    /* Here, not in worker_exit: the engine's last page requests are served meanwhile.
     */
    engine_stop();
    chdb_pagestore_shutdown();
    ereport(
        LOG, errmsg("chdb_search: worker for database %u shutting down", worker_dboid)
    );
    proc_exit(0);
}
