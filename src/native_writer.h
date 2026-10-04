#ifndef CHDB_NATIVE_WRITER_H
#define CHDB_NATIVE_WRITER_H

#include "postgres.h"

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

#endif /* CHDB_NATIVE_WRITER_H */
