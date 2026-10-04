#ifndef CHDB_NATIVE_WRITER_H
#define CHDB_NATIVE_WRITER_H

#include "postgres.h"

#include "fmgr.h"

#include "pg-clickhouse-encode.h"

/*
 * A writer over the columns `structure` declares, a Native structure clause
 * such as "ctid UInt64, body Nullable(String)", in `cxt`. ClickHouse matches a
 * Native block's columns to the target by name and rejects one it cannot find,
 * so a block carries the declared names and types, not a relation's. `ncols`,
 * if given, takes the column count.
 */
extern pgch_writer*
chdb_writer_for(MemoryContext cxt, const char* structure, size_t* ncols);

/*
 * Another extension's say in the ClickHouse type a Postgres type crosses as,
 * for a type pg-clickhouse-c does not map, such as pgvector's vector, which
 * crosses as Array(Float32). Returns the type, palloc'd, or NULL to leave
 * `typid` to the hook installed before it and at last to pgch_ch_type_for.
 * The values cross by the cast pg-clickhouse-c finds between `typid` and
 * the Postgres type of the ClickHouse one, so a mapping needs no encoder.
 */
typedef char* (*chdb_ch_type_hook)(Oid typid, int32 typmod, bool notnull);

/* The rendezvous variable holding the hook installed last. */
#define CHDB_CH_TYPE_HOOK "chdb_ch_type_hook"

/*
 * Installs `hook` for the backend in front of any installed already, and
 * returns the one it displaces, for `hook` to call on a type it leaves. The
 * hook lives in a rendezvous variable, so that every module linking a copy
 * of the codec sees the same one; inline, so that a module can install one
 * without linking the codec at all.
 */
static inline chdb_ch_type_hook
chdb_set_ch_type_hook(chdb_ch_type_hook hook) {
    void** slot            = find_rendezvous_variable(CHDB_CH_TYPE_HOOK);
    chdb_ch_type_hook prev = (chdb_ch_type_hook)*slot;

    *slot = (void*)hook;
    return prev;
}

/* The ClickHouse type `typid` crosses as: a hook's, else pgch_ch_type_for's. */
extern char*
chdb_ch_type_for(Oid typid, int32 typmod, bool notnull);

#endif /* CHDB_NATIVE_WRITER_H */
