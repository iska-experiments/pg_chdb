/*
 * The chdb_search background worker's lifecycle: one per database, claiming
 * its registry slot, opening the chDB store and socket, serving until told to
 * stop. See protocol.h for the framing and dev/design/chdb_search.md for why
 * there is a worker at all.
 */

#include "postgres.h"

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "utils/guc.h"

#include "engine.h"
#include "registry.h"
#include "serve.h"
#include "worker.h"

static Oid worker_dboid;
static bool slot_claimed;

static void
worker_exit(int code pg_attribute_unused(), Datum arg pg_attribute_unused()) {
    chdb_search_unlisten();
    chdb_search_close_store();
    if (slot_claimed) {
        chdb_search_free_slot(worker_dboid, MyProcPid);
    }
}

void
chdb_search_worker_main(Datum arg) {
    worker_dboid = DatumGetObjectId(arg);

    pqsignal(SIGHUP, SignalHandlerForConfigReload);
    pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
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

    chdb_search_load_libchdb();
    chdb_search_listen(worker_dboid);
    chdb_search_open_store(worker_dboid);
    ereport(LOG, errmsg("chdb_search: worker for database %u listening", worker_dboid));

    chdb_search_serve();

    ereport(
        LOG, errmsg("chdb_search: worker for database %u shutting down", worker_dboid)
    );
    proc_exit(0);
}
