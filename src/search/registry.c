/*
 * Registry of chdb_search workers and their registration: which database has
 * a worker, and starting one on demand from a backend.
 */

#include "postgres.h"

#include <errno.h>
#include <signal.h>

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/dsm_registry.h"
#include "utils/timestamp.h"

#include "registry.h"
#include "worker.h"

#define CHDB_SEARCH_REGISTRY_NAME "chdb_search"

/* GetNamedDSMSegment passes its init callback an argument from PG 19. */
static void
#if PG_VERSION_NUM >= 190000
registry_init(void* ptr, void* arg pg_attribute_unused()) {
#else
registry_init(void* ptr) {
#endif
    workerRegistry* reg = ptr;

    SpinLockInit(&reg->lock);
    memset(reg->slots, 0, sizeof(reg->slots));
}

workerRegistry*
chdb_search_registry(void) {
    bool found;

#if PG_VERSION_NUM >= 190000
    return GetNamedDSMSegment(
        CHDB_SEARCH_REGISTRY_NAME, sizeof(workerRegistry), registry_init, &found, NULL
    );
#else
    return GetNamedDSMSegment(
        CHDB_SEARCH_REGISTRY_NAME, sizeof(workerRegistry), registry_init, &found
    );
#endif
}

workerSlot*
chdb_search_find_slot(workerRegistry* reg, Oid dboid) {
    workerSlot* free = NULL;

    for (int i = 0; i < CHDB_SEARCH_MAX_WORKERS; i++) {
        workerSlot* slot = &reg->slots[i];

        if (slot->state != WORKER_FREE && slot->dboid == dboid) {
            return slot;
        }
        if (slot->state == WORKER_FREE && !free) {
            free = slot;
        }
    }

    return free;
}

bool
chdb_search_pid_alive(pid_t pid) {
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

void
chdb_search_free_slot(Oid dboid, pid_t only_pid) {
    workerRegistry* reg = chdb_search_registry();

    SpinLockAcquire(&reg->lock);
    workerSlot* slot = chdb_search_find_slot(reg, dboid);
    if (slot && slot->state != WORKER_FREE &&
        (only_pid == 0 || slot->pid == only_pid)) {
        slot->state = WORKER_FREE;
        slot->pid   = 0;
    }
    SpinLockRelease(&reg->lock);
}

void
chdb_search_worker_ensure(Oid dboid) {
    workerRegistry* reg = chdb_search_registry();
    TimestampTz now     = GetCurrentTimestamp();
    bool mine           = false;
    bool full           = false;

    SpinLockAcquire(&reg->lock);
    workerSlot* slot = chdb_search_find_slot(reg, dboid);
    if (!slot) {
        full = true;
    } else if (slot->state == WORKER_RUNNING && chdb_search_pid_alive(slot->pid)) {
        /* Up. The caller's connect failing means it is about to listen. */
    } else if (
        slot->state == WORKER_STARTING &&
        !TimestampDifferenceExceeds(slot->since, now, chdb_search_worker_timeout * 1000)
    ) {
        /* Another backend is starting it. */
    } else {
        slot->dboid = dboid;
        slot->state = WORKER_STARTING;
        slot->pid   = 0;
        slot->since = now;
        mine        = true;
    }
    SpinLockRelease(&reg->lock);

    if (full) {
        ereport(
            ERROR,
            errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
            errmsg("chdb_search: too many databases have a worker"),
            errdetail(
                "At most %d databases can use chdb_search at once.",
                CHDB_SEARCH_MAX_WORKERS
            )
        );
    }
    if (!mine) {
        return;
    }

    /* A hot standby serves the index read-only, so the worker starts there too. */
    BackgroundWorker bgw = {
        .bgw_flags      = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION,
        .bgw_start_time = BgWorkerStart_ConsistentState,
        .bgw_restart_time = 5,
        .bgw_main_arg     = ObjectIdGetDatum(dboid),
        .bgw_notify_pid   = MyProcPid,
    };
    BackgroundWorkerHandle* handle;
    pid_t pid;

    snprintf(bgw.bgw_library_name, BGW_MAXLEN, "chdb_search");
    snprintf(bgw.bgw_function_name, BGW_MAXLEN, "chdb_search_worker_main");
    snprintf(bgw.bgw_name, BGW_MAXLEN, "chdb_search worker for database %u", dboid);
    snprintf(bgw.bgw_type, BGW_MAXLEN, "chdb_search worker");

    if (!RegisterDynamicBackgroundWorker(&bgw, &handle)) {
        chdb_search_free_slot(dboid, 0);
        ereport(
            ERROR,
            errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
            errmsg("chdb_search: could not register the worker"),
            errhint("Increase max_worker_processes.")
        );
    }

    BgwHandleStatus status = WaitForBackgroundWorkerStartup(handle, &pid);
    if (status != BGWH_STARTED) {
        chdb_search_free_slot(dboid, 0);
        ereport(
            ERROR,
            errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
            errmsg("chdb_search: the worker did not start"),
            errhint("See the server log for why.")
        );
    }
}
