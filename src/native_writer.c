/*
 * The Native block writer over a structure clause, shared by COPY (native_send.c)
 * and the modules that link this code, so that a block's columns are named and
 * typed one way wherever one is built, and the ClickHouse type a Postgres type
 * crosses as, with the hook other extensions map their own types through.
 */

#include "postgres.h"

#include <string.h>

#include "utils/memutils.h"

#include "pg-clickhouse-encode.h"
#include "pg-clickhouse.h"

#include "native_writer.h"

/*
 * A structure clause is a named Tuple's field list, so clickhouse-c's type
 * parser splits it: quoting, nesting and Enum8('a' = 1, 'b' = 2) come for free.
 * The children belong to the Tuple, kept in `cxt` as the writer borrows them.
 */
pgch_writer*
chdb_writer_for(MemoryContext cxt, const char* structure, size_t* ncols) {
    MemoryContext old = MemoryContextSwitchTo(cxt);
    char* tuple       = psprintf("Tuple(%s)", structure);
    chc_type* type;
    chc_err err = {};

    if (chc_type_parse(tuple, strlen(tuple), &pgch_alloc, &type, &err) != CHC_OK) {
        pgch_raise(&err, ERRCODE_INVALID_PARAMETER_VALUE, "structure: ", NULL);
    }

    size_t n       = chc_type_n_children(type);
    pgch_col* cols = palloc0(n * sizeof(pgch_col));

    for (size_t i = 0; i < n; i++) {
        cols[i].name = chc_type_tuple_field_name(type, i, &cols[i].name_len);
        cols[i].type = chc_type_child(type, i);

        /* A bare type parses as an unnamed field, leaving nothing to match on. */
        if (!cols[i].name) {
            ereport(
                ERROR,
                errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("chdb: structure column %zu has no name", i + 1)
            );
        }
    }
    MemoryContextSwitchTo(old);
    if (ncols) {
        *ncols = n;
    }

    return pgch_writer_new(cxt, cols, n);
}

char*
chdb_ch_type_for(Oid typid, int32 typmod, bool notnull) {
    chdb_ch_type_hook hook =
        (chdb_ch_type_hook)*find_rendezvous_variable(CHDB_CH_TYPE_HOOK);
    char* type = hook ? hook(typid, typmod, notnull) : NULL;

    return type ? type : pgch_ch_type_for(typid, typmod, notnull, NULL);
}
