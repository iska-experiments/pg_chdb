/*
 * The chdb_hook module: a process utility hook that takes over a COPY or a
 * CREATE TABLE naming a URL chDB supports, with the checks DoCopy() would
 * apply (relation.c) and the statement's options (options.c), and leaves
 * every other statement to Postgres.
 */

#include "postgres.h"

#include "access/table.h"
#include "miscadmin.h"
#include "tcop/utility.h"

#include "../gucs.h"
#include "../module.h"
#include "copy.h"
#include "create.h"
#include "options.h"
#include "relation.h"
#include "url.h"

CHDB_MODULE_MAGIC("chdb_hook");

void
InitializeUtilityHook(void);

/* previous hook */
static ProcessUtility_hook_type PrevProcessUtility = NULL;

/* The chDB process utility hook. */
static void
chDBProcessUtilityHook(
    PlannedStmt* plannedStmt,
    const char* queryString,
    bool readOnlyTree,
    ProcessUtilityContext context,
    ParamListInfo params,
    struct QueryEnvironment* queryEnv,
    DestReceiver* dest,
    QueryCompletion* completionTag
);

/*
 * GUCs for settings to be passed to chDB, referenced by CHDB_GUCS().
 */
static int chdb_max_memory  = 0;
static int chdb_max_threads = 0;
static int chdb_max_parsers = 0;

/*
 * _PG_init is the entry-point for the library.
 */
void
_PG_init(void);
void
_PG_init(void) {
    if (IsBinaryUpgrade) {
        return;
    }

    CHDB_GUCS("chdb_hook");
    InitializeUtilityHook();
}

/*
 * InitializeUtilityHook hooks chDBProcessUtilityHook into the process utility
 * hook to in order to intercept DDL commands.
 */
void
InitializeUtilityHook(void) {
    PrevProcessUtility =
        ProcessUtility_hook ? ProcessUtility_hook : standard_ProcessUtility;
    ProcessUtility_hook = chDBProcessUtilityHook;
}

/*
 * chDBProcessUtilityHook modifies the behaviour of DDL commands.
 */
static void
chDBProcessUtilityHook(
    PlannedStmt* plannedStmt,
    const char* queryString,
    bool readOnlyTree,
    ProcessUtilityContext context,
    ParamListInfo params,
    struct QueryEnvironment* queryEnv,
    DestReceiver* dest,
    QueryCompletion* qc
) {
    Node* parsetree = plannedStmt->utilityStmt;

    /* Is this a COPY statement? */
    if (IsA(parsetree, CopyStmt)) {
        /* Look for a URL filename. */
        CopyStmt* copy = (CopyStmt*)parsetree;
        scheme scheme  = chdb_url_scheme(copy->filename);

        /* Leave COPY TO/FROM PROGRAM to Postgres, which gates it on a role. */
        if (copy->relation && !copy->is_program && scheme != no_scheme) {
            if (copy->whereClause) {
                ereport(
                    ERROR,
                    errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("chdb: COPY FROM a URL not supported with a WHERE clause")
                );
            }

            /* We own this copy, but only if the user may copy the relation. */
            if (copy->is_from) {
                PreventCommandIfReadOnly("COPY FROM");
            }
            if (scheme == file_scheme) {
                chdb_check_server_file_privileges(copy->is_from);
            }
            chdbCopyContext ctx = {
                .scheme      = scheme,
                .cmd_type    = copy->is_from ? CHDB_CMD_SELECT : CHDB_CMD_INSERT,
                .url         = copy->filename,
                .max_memory  = (uint16_t)chdb_max_memory,
                .max_threads = (uint16_t)chdb_max_threads,
                .max_parsers = (uint16_t)chdb_max_parsers,
            };

            chdb_open_copy_relation(copy, &ctx);
            chdb_copy_options(&ctx, copy->options, NULL);
            SetQueryCompletion(qc, CMDTAG_COPY, chdb_copy(&ctx));

            /* Retain the lock until commit, so the copy is what we checked. */
            table_close(ctx.rel, NoLock);

            return;
        }
    }

    /* Handle CREATE TABLE using URL for columns or rows */
    if (IsA(parsetree, CreateStmt) && chdb_creates_from_url((CreateStmt*)parsetree)) {
        /* Copy read-only parse tree before modifying statement */
        if (readOnlyTree) {
            plannedStmt  = copyObject(plannedStmt);
            parsetree    = plannedStmt->utilityStmt;
            readOnlyTree = false;
        }

        CreateStmt* create     = (CreateStmt*)parsetree;
        chdbCreateFromURL from = {};
        chdbCopyContext ctx    = {
            .max_memory  = (uint16_t)chdb_max_memory,
            .max_threads = (uint16_t)chdb_max_threads,
            .max_parsers = (uint16_t)chdb_max_parsers,
        };

        /* Reject read-only transaction before requesting remote schema */
        PreventCommandIfReadOnly("CREATE TABLE");
        chdb_create_columns_from_url(create, &from, &ctx);
        PrevProcessUtility(
            plannedStmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc
        );
        if (from.copy_url) {
            chdb_copy_url_into(create->relation, from.copy_url, &ctx);
        }

        return;
    }

    /* Continue with the internal execution. */
    PrevProcessUtility(
        plannedStmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc
    );
}
