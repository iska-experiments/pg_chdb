/*
 * Support CREATE TABLE using columns and optional rows from URL supported by chDB
 * Get columns from explicit structure or infer them with DESCRIBE, and copy the
 * rows into the new relation
 */

#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "utils/lsyscache.h"

#include "pg-clickhouse.h"

#include "../native.h"
#include "create.h"
#include "options.h"
#include "relation.h"
#include "url.h"

/* Remove `name` from `create` options and return its value, or NULL if absent */
static char*
take_url_option(CreateStmt* create, const char* name) {
    ListCell* lc;

    foreach (lc, create->options) {
        DefElem* elem = lfirst(lc);

        if (strcmp(elem->defname, name) == 0) {
            create->options = list_delete_cell(create->options, lc);

            return defGetString(elem);
        }
    }

    return NULL;
}

bool
chdb_creates_from_url(CreateStmt* create) {
    ListCell* lc;

    foreach (lc, create->options) {
        DefElem* elem = lfirst(lc);

        if (strcmp(elem->defname, CHDB_STRUCTURE_FROM) == 0 ||
            strcmp(elem->defname, CHDB_COPY_FROM) == 0) {
            return true;
        }
    }

    return false;
}

/*
 * Report existing relation before requesting remote schema
 * Reject IF NOT EXISTS because subsequent copy requires newly created relation
 */
static void
error_if_relation_exists(CreateStmt* create) {
    Oid nspid = RangeVarGetCreationNamespace(create->relation);

    if (!OidIsValid(get_relname_relid(create->relation->relname, nspid))) {
        return;
    }
    if (create->if_not_exists) {
        ereport(
            ERROR,
            errcode(ERRCODE_DUPLICATE_TABLE),
            errmsg("relation \"%s\" already exists", create->relation->relname),
            errdetail(
                "CREATE TABLE IF NOT EXISTS neither derives columns nor copies rows "
                "from a URL."
            ),
            errhint("Use COPY to load an existing relation.")
        );
    }
    ereport(
        ERROR,
        errcode(ERRCODE_DUPLICATE_TABLE),
        errmsg("relation \"%s\" already exists", create->relation->relname)
    );
}

void
chdb_create_from_url(CreateStmt* create, chdbCreateFromURL* from) {
    from->structure_url = take_url_option(create, CHDB_STRUCTURE_FROM);
    from->copy_url      = take_url_option(create, CHDB_COPY_FROM);

    if (from->structure_url && from->copy_url) {
        ereport(
            ERROR,
            errcode(ERRCODE_SYNTAX_ERROR),
            errmsg(
                "chdb: cannot combine option \"%s\" with option \"%s\"",
                CHDB_STRUCTURE_FROM,
                CHDB_COPY_FROM
            ),
            errdetail(
                "\"%s\" derives the columns from the URL it loads.", CHDB_COPY_FROM
            )
        );
    }

    /* Partitions, inherited tables, typed tables, and column lists define columns */
    bool has_columns = create->tableElts != NIL || create->inhRelations != NIL ||
                       create->partbound || create->ofTypename;

    if (from->structure_url && has_columns) {
        ereport(
            ERROR,
            errcode(ERRCODE_SYNTAX_ERROR),
            errmsg(
                "chdb: option \"%s\" requires a table that names no columns",
                CHDB_STRUCTURE_FROM
            ),
            errdetail(
                "A column list, an INHERITS clause, an OF type, or a partition each "
                "define columns."
            )
        );
    }
    if (from->copy_url && !has_columns) {
        from->structure_url = from->copy_url;
    }

    error_if_relation_exists(create);
}

List*
chdb_url_columns(chdbCopyContext* ctx) {
    List* columns = NIL;
    ListCell* lc;
    bool infer_structure =
        ctx->structure[0] == '\0' || strcmp(ctx->structure, "auto") == 0;
    StringInfoData structure;

    initStringInfo(&structure);

    foreach (lc, chdb_describe(ctx)) {
        chdbDescribedColumn* described = lfirst(lc);
        const char* where              = psprintf("column \"%s\"", described->name);
        chc_type* parsed;
        chc_err err = {};

        if (chc_type_parse(
                described->type, strlen(described->type), &pgch_alloc, &parsed, &err
            ) != CHC_OK) {
            pgch_raise(&err, ERRCODE_FEATURE_NOT_SUPPORTED, NULL, where);
        }

        pgch_pg_type type = pgch_pg_type_for(parsed, where);

        if (infer_structure) {
            if (structure.len) {
                appendStringInfoString(&structure, ", ");
            }
            appendStringInfo(
                &structure,
                "%s %s",
                pgch_quote_ch_ident(described->name),
                described->type
            );
            if (chc_type_kind(parsed) == CHC_NESTED) {
                ctx->preserve_nested = true;
            }
        }

        /* Convert pseudo types to text arrays valid in table columns */
        if (OidIsValid(type.typid) && !pgch_pg_type_is_column(type)) {
            const char* decl = "text[]";

            type.typid  = TEXTARRAYOID;
            type.typmod = -1;
            type.ndims++;
            for (int dim = 1; dim < type.ndims; dim++) {
                decl = psprintf("%s[]", decl);
            }
            ereport(
                NOTICE,
                errmsg(
                    "chdb: column \"%s\" of type \"%s\" converted to %s",
                    described->name,
                    described->type,
                    decl
                )
            );
        }

        /* Reject pseudo types because table columns cannot use them */
        if (!pgch_pg_type_is_column(type)) {
            ereport(
                ERROR,
                errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg(
                    "chdb: no Postgres type for column \"%s\" of type \"%s\"",
                    described->name,
                    described->type
                ),
                errhint("Use a \"structure\" option that maps the column to String.")
            );
        }

        ColumnDef* column =
            makeColumnDef(described->name, type.typid, type.typmod, InvalidOid);

        /* One PG array type spans every depth, so attndims keeps ClickHouse nesting */
        for (int dim = 0; dim < type.ndims; dim++) {
            column->typeName->arrayBounds =
                lappend(column->typeName->arrayBounds, makeInteger(-1));
        }

        /* Apply nullability reported by ClickHouse */
        column->is_not_null = !type.nullable;
        columns             = lappend(columns, column);
    }

    if (columns == NIL) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("chdb: no columns found at \"%s\"", ctx->url)
        );
    }

    if (infer_structure) {
        StringInfoData param;
        initStringInfo(&param);

        /* chDB unescapes String query parameters before parsing structure */
        for (const char* p = structure.data; *p; p++) {
            if (*p == '\\') {
                appendStringInfoChar(&param, '\\');
            }
            appendStringInfoChar(&param, *p);
        }
        ctx->structure = param.data;
    }
    pfree(structure.data);

    return columns;
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
        chdb_check_server_file_privileges(true);
    }

    return scheme;
}

/*
 * Add columns inferred from URL to `create`
 * Store URL options in `from` and chDB options in `ctx` for subsequent copy
 */
void
chdb_create_columns_from_url(
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
void
chdb_copy_url_into(RangeVar* relation, char* url, chdbCopyContext* ctx) {
    CopyStmt* copy = makeNode(CopyStmt);

    copy->relation = relation;
    copy->is_from  = true;
    copy->filename = url;

    ctx->url      = url;
    ctx->scheme   = option_url_scheme(url, CHDB_COPY_FROM);
    ctx->cmd_type = CHDB_CMD_SELECT;

    /* Make newly created relation visible */
    CommandCounterIncrement();
    chdb_open_copy_relation(copy, ctx);
    chdb_copy(ctx);

    /* Keep relation lock until transaction ends */
    table_close(ctx->rel, NoLock);
}
