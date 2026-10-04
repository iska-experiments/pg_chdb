/*
 * Handle COPY and CREATE TABLE commands that use URLs supported by chDB
 * Apply validation performed by PostgreSQL DoCopy()
 */

#include "postgres.h"

#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/rel.h"
#include "utils/rls.h"

#include "../gucs.h"
#include "../module.h"
#include "copy.h"
#include "create.h"
#include "options.h"
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
 * A file:// URL reads and writes files on the server, which Postgres gates on
 * membership in a role. Apply the same gate as DoCopy() does.
 */
static void
check_server_file_privileges(bool is_from) {
    if (is_from) {
        if (!has_privs_of_role(GetUserId(), ROLE_PG_READ_SERVER_FILES)) {
            ereport(
                ERROR,
                errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                errmsg("chdb: permission denied to COPY from a file"),
                errdetail(
                    "Only roles with privileges of the \"pg_read_server_files\" role "
                    "may COPY from a file."
                )
            );
        }
    } else if (!has_privs_of_role(GetUserId(), ROLE_PG_WRITE_SERVER_FILES)) {
        ereport(
            ERROR,
            errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
            errmsg("chdb: permission denied to COPY to a file"),
            errdetail(
                "Only roles with privileges of the \"pg_write_server_files\" role may "
                "COPY to a file."
            )
        );
    }
}

/*
 * Opens and locks relation named by COPY statement, with privilege checks that
 * DoCopy() applies to a normal COPY: INSERT or SELECT on relation or on each
 * copied column, then row-level security. Errors out unless the current user
 * may copy the relation. Fills `ctx` with the locked relation and the columns
 * it checked; the caller must close the relation.
 */
static void
open_copy_relation(CopyStmt* copy, chdbCopyContext* ctx) {
    LOCKMODE lockmode = copy->is_from ? RowExclusiveLock : AccessShareLock;
    Relation rel      = table_openrv(copy->relation, lockmode);

    ParseState* pstate = make_parsestate(NULL);
    ParseNamespaceItem* nsitem =
        addRangeTableEntryForRelation(pstate, rel, lockmode, NULL, false, false);
#if PG_VERSION_NUM >= 160000
    RTEPermissionInfo* perminfo = nsitem->p_perminfo;
    perminfo->requiredPerms     = copy->is_from ? ACL_INSERT : ACL_SELECT;
#else
    /* Delete test/expected/permissions_1.out when Postgres 15 dropped. */
    RangeTblEntry* perminfo = nsitem->p_rte;
    perminfo->requiredPerms = (copy->is_from ? ACL_INSERT : ACL_SELECT);
#endif

    ctx->rel     = rel;
    ctx->attnums = CopyGetAttnums(RelationGetDescr(rel), rel, copy->attlist);

    /* Only the copied columns require privileges. */
    Bitmapset** cols =
        copy->is_from ? &perminfo->insertedCols : &perminfo->selectedCols;
    ListCell* lc;
    foreach (lc, ctx->attnums) {
        *cols =
            bms_add_member(*cols, lfirst_int(lc) - FirstLowInvalidHeapAttributeNumber);
    }
#if PG_VERSION_NUM >= 160000
    ExecCheckPermissions(pstate->p_rtable, pstate->p_rteperminfos, true);
#else
    ExecCheckRTPerms(pstate->p_rtable, true);
#endif

    /* A COPY FROM hands this to the executor rather than building its own. */
    ctx->rtable = pstate->p_rtable;
#if PG_VERSION_NUM >= 160000
    ctx->rteperminfos = pstate->p_rteperminfos;
#endif

    /*
     * chDB copies the whole relation, so policies cannot be applied to the
     * rows. Postgres runs a query-based COPY TO, which we don't yet support.
     */
    if (check_enable_rls(RelationGetRelid(rel), InvalidOid, false) == RLS_ENABLED) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg(
                "chdb: COPY %s not supported with row-level security",
                copy->is_from ? "FROM" : "TO"
            ),
            errdetail(
                "Row-level security policies apply to relation \"%s\" for this role.",
                RelationGetRelationName(rel)
            )
        );
    }

    /*
     * COPY TO scans storage directly, so reject what Postgres rejects for the
     * relation form of the command. A COPY FROM target is checked by
     * CheckValidResultRel once the executor state exists.
     */
    if (!copy->is_from && rel->rd_rel->relkind != RELKIND_RELATION) {
        ereport(
            ERROR,
            errcode(ERRCODE_WRONG_OBJECT_TYPE),
            errmsg(
                "chdb: cannot copy from relation \"%s\"", RelationGetRelationName(rel)
            ),
            errdetail_relkind_not_supported(rel->rd_rel->relkind)
        );
    }
}

/*
 * Parse URL from CREATE TABLE option
 * Reject unsupported schemes and check server file privileges for file URLs
 */
static scheme
option_url_scheme(const char* url, const char* option) {
    scheme scheme = chdb_url_scheme(url);

    if (scheme == no_scheme) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg(
                "chdb: cannot read URL \"%s\" specified by option \"%s\"", url, option
            )
        );
    }
    if (scheme == file_scheme) {
        check_server_file_privileges(true);
    }

    return scheme;
}

/*
 * Add columns inferred from URL to `create`
 * Store URL options in `from` and chDB options in `ctx` for subsequent copy
 */
static void
create_columns_from_url(
    CreateStmt* create,
    chdbCreateFromURL* from,
    chdbCopyContext* ctx
) {
    List* storage = NIL;

    chdb_create_from_url(create, from);

    /* Pass data options to chDB and keep PostgreSQL storage parameters */
    chdb_copy_options(ctx, create->options, &storage);
    create->options = storage;

    if (from->structure_url) {
        ctx->url          = from->structure_url;
        ctx->scheme       = option_url_scheme(ctx->url, CHDB_STRUCTURE_FROM);
        create->tableElts = chdb_url_columns(ctx);
    }
}

/* Copy rows from `url` into newly created `relation` */
static void
copy_url_into(RangeVar* relation, char* url, chdbCopyContext* ctx) {
    CopyStmt* copy = makeNode(CopyStmt);

    copy->relation = relation;
    copy->is_from  = true;
    copy->filename = url;

    ctx->url      = url;
    ctx->scheme   = option_url_scheme(url, CHDB_COPY_FROM);
    ctx->cmd_type = CHDB_CMD_SELECT;

    /* Make newly created relation visible */
    CommandCounterIncrement();
    open_copy_relation(copy, ctx);
    chdb_copy(ctx);

    /* Keep relation lock until transaction ends */
    table_close(ctx->rel, NoLock);
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
                check_server_file_privileges(copy->is_from);
            }
            chdbCopyContext ctx = {
                .scheme      = scheme,
                .cmd_type    = copy->is_from ? CHDB_CMD_SELECT : CHDB_CMD_INSERT,
                .url         = copy->filename,
                .max_memory  = (uint16_t)chdb_max_memory,
                .max_threads = (uint16_t)chdb_max_threads,
                .max_parsers = (uint16_t)chdb_max_parsers,
            };

            open_copy_relation(copy, &ctx);
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
        create_columns_from_url(create, &from, &ctx);
        PrevProcessUtility(
            plannedStmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc
        );
        if (from.copy_url) {
            copy_url_into(create->relation, from.copy_url, &ctx);
        }

        return;
    }

    /* Continue with the internal execution. */
    PrevProcessUtility(
        plannedStmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc
    );
}
