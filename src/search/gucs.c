/*
 * The access method's GUCs, the DEBUG1 statement log used by every file that
 * talks to the worker, and registration of the transaction callbacks. The
 * module entry point in search.c runs chdb_search_am_init.
 */

#include "postgres.h"

#include <ctype.h>

#include "utils/guc.h"

#include "search.h"

int chdb_search_flush_threshold_kb       = 64 * 1024;
int chdb_search_max_buffer_kb            = 1024 * 1024;
double chdb_search_vacuum_optimize_ratio = 0.2;
bool chdb_search_mask_oids               = false;
int chdb_search_unavailable_index        = CHDB_UNAVAILABLE_ERROR;

static const struct config_enum_entry unavailable_index_options[] = {
    { "error", CHDB_UNAVAILABLE_ERROR, false },
    { "skip",  CHDB_UNAVAILABLE_SKIP,  false },
    { NULL,    0,                      false },
};

/*
 * Prefixes whose run of digits the mask replaces: OID, generation (in a
 * table name, a literal and a predicate), xid, and the generation and LSN of
 * a meta row, a list of numbers.
 */
static const char* const masked_prefixes[] = {
    "idx_", ".t_", "'t_", "_tx_", "generation = ", "VALUES (",
};

/*
 * The statement as the log and EXPLAIN show it: as is, or with mask_oids
 * the index OIDs, store generations, transaction ids and WAL positions
 * replaced by N, as they differ from run to run and tests compare the text.
 */
const char*
chdb_search_mask_sql(const char* sql) {
    StringInfoData buf;

    if (!chdb_search_mask_oids) {
        return sql;
    }
    initStringInfo(&buf);
    for (const char* p = sql; *p;) {
        size_t len = 0;

        for (int i = 0; i < lengthof(masked_prefixes) && !len; i++) {
            if (strncmp(p, masked_prefixes[i], strlen(masked_prefixes[i])) == 0) {
                len = strlen(masked_prefixes[i]);
            }
        }
        if (len) {
            appendBinaryStringInfo(&buf, p, len);
            p += len;
            while (isdigit((unsigned char)*p)) {
                while (isdigit((unsigned char)*p)) {
                    p++;
                }
                appendStringInfoChar(&buf, 'N');
                if (strncmp(p, ", ", 2) != 0 || !isdigit((unsigned char)p[2])) {
                    break;
                }
                appendStringInfoString(&buf, ", ");
                p += 2;
            }
        } else {
            appendStringInfoChar(&buf, *p++);
        }
    }
    return buf.data;
}

/*
 * Logs a ClickHouse statement at DEBUG1, so tests can assert on what was
 * generated with client_min_messages = debug1.
 */
void
chdb_search_log_sql(const char* what, const char* sql) {
    elog(DEBUG1, "chdb_search %s: %s", what, chdb_search_mask_sql(sql));
}

void
chdb_search_am_init(void) {
    DefineCustomIntVariable(
        "chdb_search.flush_threshold",
        "Bytes of insert buffer per index above which a transaction stages rows in "
        "ClickHouse.",
        "Rows inserted inside a savepoint are not staged: their buffer grows until "
        "COMMIT, up to chdb_search.max_buffer.",
        &chdb_search_flush_threshold_kb,
        64 * 1024,
        64,
        MAX_KILOBYTES,
        PGC_USERSET,
        GUC_UNIT_KB,
        NULL,
        NULL,
        NULL
    );
    DefineCustomIntVariable(
        "chdb_search.max_buffer",
        "Bytes of insert buffer per index above which an insert fails.",
        "A ceiling for rows that cannot be staged early, so that a transaction gets "
        "an error rather than the backend an OOM kill. Zero means no limit.",
        &chdb_search_max_buffer_kb,
        1024 * 1024,
        0,
        MAX_KILOBYTES,
        PGC_USERSET,
        GUC_UNIT_KB,
        NULL,
        NULL,
        NULL
    );
    DefineCustomRealVariable(
        "chdb_search.vacuum_optimize_ratio",
        "Fraction of dead index entries above which VACUUM runs OPTIMIZE TABLE FINAL.",
        NULL,
        &chdb_search_vacuum_optimize_ratio,
        0.2,
        0.0,
        1.0,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    DefineCustomBoolVariable(
        "chdb_search.mask_oids",
        "Replace OIDs and transaction ids in logged ClickHouse statements by N, for "
        "tests.",
        NULL,
        &chdb_search_mask_oids,
        false,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    DefineCustomEnumVariable(
        "chdb_search.unavailable_index",
        "What a scan does when a chdb index's store is not available on this server.",
        "error raises so a broken index is never silent; skip lets the planner use "
        "another path.",
        &chdb_search_unavailable_index,
        CHDB_UNAVAILABLE_ERROR,
        unavailable_index_options,
        PGC_USERSET,
        0,
        NULL,
        NULL,
        NULL
    );
    chdb_search_init_options();
    chdb_search_init_insert();
    chdb_search_init_drop();
}
