#ifndef CHDB_OPTIONS_H
#define CHDB_OPTIONS_H

#include "postgres.h"

#include "nodes/pg_list.h"

#include "copy.h"

/*
 * Fills `ctx` from `options`, the DefElems of a COPY or CREATE TABLE, taking
 * their defaults for the options absent. Collects the options chDB does not
 * use in `*others`, or rejects them when `others` is NULL.
 */
extern void
chdb_copy_options(chdbCopyContext* ctx, List* options, List** others);

#endif /* CHDB_OPTIONS_H */
