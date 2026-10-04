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

/* Prefixes whose run of digits the mask replaces: OID, generation, xid. */
static const char* const masked_prefixes[] = { "idx_", ".t_", "_tx_" };

/*
 * Logs a ClickHouse statement at DEBUG1, so tests can assert on what was
 * generated with client_min_messages = debug1. With mask_oids, index OIDs,
 * store generations and transaction ids become N, as they differ from run to
 * run.
 */
void
chdb_search_log_sql(const char* what, const char* sql) {
    StringInfoData buf;

    if (!chdb_search_mask_oids) {
        elog(DEBUG1, "chdb_search %s: %s", what, sql);
        return;
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
            if (isdigit((unsigned char)*p)) {
                while (isdigit((unsigned char)*p)) {
                    p++;
                }
                appendStringInfoChar(&buf, 'N');
            }
        } else {
            appendStringInfoChar(&buf, *p++);
        }
    }
    elog(DEBUG1, "chdb_search %s: %s", what, buf.data);
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
    chdb_search_init_options();
    chdb_search_init_insert();
    chdb_search_init_drop();
}
