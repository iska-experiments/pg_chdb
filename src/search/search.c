/*
 * Module entry point for chdb_search: the worker's GUCs, the access method's
 * initialization and the version function.
 */

#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "tcop/cmdtag.h"
#include "tcop/pquery.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "../gucs.h"
#include "../module.h"
#include "search.h"
#include "worker.h"

CHDB_MODULE_MAGIC("chdb_search");

int chdb_max_memory  = 0;
int chdb_max_threads = 0;
int chdb_max_parsers = 0;

int chdb_search_worker_timeout = 30;

void
_PG_init(void);
void
_PG_init(void) {
    if (IsBinaryUpgrade) {
        return;
    }

    DefineCustomIntVariable(
        "chdb_search.worker_timeout",
        "Seconds to wait for the chdb_search worker to start.",
        NULL,
        &chdb_search_worker_timeout,
        30,
        1,
        3600,
        PGC_USERSET,
        GUC_UNIT_S,
        NULL,
        NULL,
        NULL
    );
    chdb_search_am_init();

    /*
     * Loaded on demand by a DROP (or an ALTER that drops), this init runs from
     * index_drop's relcache build, after the object access hook for the index
     * would have fired, so that index keeps its store until the worker next
     * starts and sweeps it. Say so, once per session, as preloading is the
     * cure. Other statements load the library without dropping anything.
     */
    if (ActivePortal) {
        const char* tag = GetCommandTagName(ActivePortal->commandTag);

        if (strncmp(tag, "DROP ", 5) == 0 || strncmp(tag, "ALTER ", 6) == 0) {
            ereport(
                LOG,
                errmsg("chdb_search was loaded on demand by this %s", tag),
                errdetail(
                    "A chdb index it drops keeps its store until the database's "
                    "worker next starts and sweeps it."
                ),
                errhint(
                    "Add chdb_search to session_preload_libraries or "
                    "shared_preload_libraries."
                )
            );
        }
    }

    /* Last: reserving the prefix drops placeholders for GUCs not yet defined. */
    CHDB_GUCS("chdb_search");
}

PG_FUNCTION_INFO_V1(chdb_search_version);
Datum
chdb_search_version(PG_FUNCTION_ARGS) {
    PG_RETURN_TEXT_P(cstring_to_text(PGCHCB_VERSION));
}
