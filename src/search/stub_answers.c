/*
 * What the stub worker client answers a select with: one Native block,
 * encoded as the row writer encodes rows, so the access method reads it as
 * it would the worker's. The statement's shape picks the answer (stub.h);
 * the GUCs of client_stub.c supply the values.
 */

#include "postgres.h"

#include <string.h>

#include "access/genam.h"
#include "catalog/pg_type_d.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "pg-clickhouse-encode.h"
#include "pg-clickhouse.h"

#include "../native_writer.h"
#include "search.h"
#include "stub.h"

/* A block's writer, in a context of its own that take_block deletes. */
static pgch_writer*
block_writer(const char* structure, MemoryContext* cxt, MemoryContext* old) {
    *cxt = AllocSetContextCreate(
        CurrentMemoryContext, "chdb_search stub block", ALLOCSET_SMALL_SIZES
    );
    *old = MemoryContextSwitchTo(*cxt);
    return chdb_writer_for(*cxt, structure, NULL);
}

/* The writer's rows as one block in the caller's context; `cxt` goes. */
static size_t
take_block(pgch_writer* w, MemoryContext cxt, MemoryContext old, void** out) {
    pgch_buf buf = {};

    if (pgch_writer_rows(w)) {
        pgch_writer_flush(w, &buf, NULL);
    }
    MemoryContextSwitchTo(old);
    *out = palloc(buf.len + 1);
    memcpy(*out, buf.data, buf.len);
    MemoryContextDelete(cxt);
    return buf.len;
}

/* Counts the columns a scan's statement selects under an alias. */
static int
count_aliases(const char* sql, const char* alias) {
    int n = 0;

    for (const char* p = sql; (p = strstr(p, alias)); p += 1) {
        n++;
    }
    return n;
}

/*
 * The ctids GUC as one block, with `ndist` Float64 distance columns and
 * `nscores` Float32 score columns, each holding the ctid. Zero for no rows.
 */
static size_t
encode_ctids(const char* ctids, int ndist, int nscores, void** out) {
    MemoryContext cxt, old;
    StringInfoData structure;

    initStringInfo(&structure);
    appendStringInfoString(&structure, "ctid UInt64");
    for (int i = 0; i < ndist; i++) {
        appendStringInfo(&structure, ", _distance%d Float64", i);
    }
    for (int i = 0; i < nscores; i++) {
        appendStringInfo(&structure, ", _score%d Float32", i);
    }

    pgch_writer* w = block_writer(structure.data, &cxt, &old);

    for (const char* p = ctids; *p;) {
        char* end;
        uint64 ctid = strtoull(p, &end, 10);

        if (end == p || (*end != ',' && *end != '\0')) {
            ereport(
                ERROR,
                errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("chdb_search stub: invalid ctid list \"%s\"", ctids)
            );
        }
        pgch_append_datum(w, 0, Int64GetDatum((int64)ctid), INT8OID, false);
        for (int i = 0; i < ndist; i++) {
            pgch_append_datum(w, 1 + i, Float8GetDatum((double)ctid), FLOAT8OID, false);
        }
        for (int i = 0; i < nscores; i++) {
            pgch_append_datum(
                w, 1 + ndist + i, Float4GetDatum((float4)ctid), FLOAT4OID, false
            );
        }
        p = *end == ',' ? end + 1 : end;
    }
    return take_block(w, cxt, old, out);
}

/*
 * The store's answer to the fail-safe check of meta.c, (flushes, last flush,
 * tables) for the index's generation: by default what the metapage says, so
 * that the check passes as it does against a store that is current.
 */
static size_t
encode_meta(Oid indexoid, void** out) {
    MemoryContext cxt, old;
    Relation index = index_open(indexoid, AccessShareLock);
    ChdbMetaPageData meta;
    uint64 rows = 1, tables = 1;

    chdb_meta_read(index, &meta);
    index_close(index, AccessShareLock);
    if (strcmp(chdb_search_stub_meta, "none") == 0) {
        rows = tables = meta.flushed_lsn = 0;
    } else if (*chdb_search_stub_meta) {
        meta.flushed_lsn = strtoull(chdb_search_stub_meta, NULL, 10);
    }

    pgch_writer* w = block_writer("n UInt64, lsn UInt64, t UInt64", &cxt, &old);

    pgch_append_datum(w, 0, Int64GetDatum((int64)rows), INT8OID, false);
    pgch_append_datum(w, 1, Int64GetDatum((int64)meta.flushed_lsn), INT8OID, false);
    pgch_append_datum(w, 2, Int64GetDatum((int64)tables), INT8OID, false);
    return take_block(w, cxt, old, out);
}

/* The end of the list item at `p`: the next comma, or the end of the list. */
static const char*
item_end(const char* p) {
    const char* comma = strchr(p, ',');

    return comma ? comma : p + strlen(p);
}

/* The tokens GUC, comma-separated, as the one row tokens() returns. */
static size_t
encode_tokens(const char* tokens, void** out) {
    MemoryContext cxt, old;
    Datum* elems = palloc(sizeof(Datum) * (strlen(tokens) + 1));
    int n        = 0;

    for (const char* p = tokens; *p;) {
        const char* end = item_end(p);

        elems[n++] = CStringGetTextDatum(pnstrdup(p, end - p));
        p          = *end ? end + 1 : end;
    }

    pgch_writer* w = block_writer("t Array(Nullable(String))", &cxt, &old);

    pgch_append_datum(
        w,
        0,
        PointerGetDatum(construct_array_builtin(elems, n, TEXTOID)),
        TEXTARRAYOID,
        false
    );
    return take_block(w, cxt, old, out);
}

static size_t
encode_count(int64 n, void** out) {
    MemoryContext cxt, old;
    pgch_writer* w = block_writer("n UInt64", &cxt, &old);

    pgch_append_datum(w, 0, Int64GetDatum(n), INT8OID, false);
    return take_block(w, cxt, old, out);
}

/* How many ctids the GUC names: the store's count() of its rows. */
static int64
count_ctids(void) {
    int64 n = *chdb_search_stub_ctids ? 1 : 0;

    for (const char* p = chdb_search_stub_ctids; (p = strchr(p, ',')); p++) {
        n++;
    }
    return n;
}

/*
 * The count a statement gets: the frequency the GUC gives the one token a
 * score's WHERE names as an array, `['tok']`, or zero for a token not
 * listed; else the ctids' number, as the aggregate scan's count(*) gets
 * however the WHERE filters the rows.
 */
static int64
count_for(const char* sql) {
    const char* where = strstr(sql, " WHERE ");
    const char* tok   = where ? strstr(where, "['") : NULL;
    const char* end   = tok ? strstr(tok + 2, "']") : NULL;

    if (!end) {
        return count_ctids();
    }
    tok += 2;
    for (const char* p = chdb_search_stub_frequencies; *p;) {
        const char* colon = strchr(p, ':');
        const char* next  = item_end(p);

        if (colon && colon < next && colon - p == end - tok &&
            strncmp(p, tok, end - tok) == 0) {
            return strtoll(colon + 1, NULL, 10);
        }
        p = *next ? next + 1 : next;
    }
    return 0;
}

size_t
chdb_stub_answer(const char* sql, Oid indexoid, void** out) {
    if (strstr(sql, ".meta WHERE generation = ")) {
        return encode_meta(indexoid, out);
    }
    if (strncmp(sql, "SELECT tokens(", 14) == 0) {
        return encode_tokens(chdb_search_stub_tokens, out);
    }
    if (strncmp(sql, "SELECT count() FROM ", 20) == 0) {
        return encode_count(count_for(sql), out);
    }
    return encode_ctids(
        chdb_search_stub_ctids,
        count_aliases(sql, " AS _distance"),
        count_aliases(sql, " AS _score"),
        out
    );
}
