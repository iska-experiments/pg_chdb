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
double chdb_search_vacuum_optimize_ratio = 0.2;
bool chdb_search_mask_oids               = false;

/*
 * Logs a ClickHouse statement at DEBUG1, so tests can assert on what was
 * generated with client_min_messages = debug1. With mask_oids, index OIDs and
 * transaction ids become N, as they differ from run to run.
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
        if (strncmp(p, "idx_", 4) == 0 || strncmp(p, "_tx_", 4) == 0) {
            appendBinaryStringInfo(&buf, p, 4);
            p += 4;
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
        NULL,
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
