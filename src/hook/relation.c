/*
 * The relation a COPY names, opened with the checks DoCopy() applies to a
 * COPY of its own: the role gate on server files, the INSERT or SELECT
 * privilege on the copied columns, row-level security, and the kinds of
 * relation a COPY TO can read.
 */

#include "postgres.h"

#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "commands/copy.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "utils/acl.h"
#include "utils/rel.h"
#include "utils/rls.h"

#include "relation.h"

/*
 * A file:// URL reads and writes files on the server, which Postgres gates on
 * membership in a role. Apply the same gate as DoCopy() does.
 */
void
chdb_check_server_file_privileges(bool is_from) {
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
void
chdb_open_copy_relation(CopyStmt* copy, chdbCopyContext* ctx) {
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
