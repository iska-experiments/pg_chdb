/*
 * Module entry point for chdb_vector.
 */

#include "postgres.h"

#include "fmgr.h"

#include "chdb_vector.h"

PG_MODULE_MAGIC_EXT(.name = "chdb_vector", .version = "0.1");

void
_PG_init(void);
void
_PG_init(void) {
    chdb_vector_define_gucs();
}
