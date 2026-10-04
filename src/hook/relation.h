#ifndef CHDB_RELATION_H
#define CHDB_RELATION_H

#include "postgres.h"

#include "nodes/parsenodes.h"

#include "copy.h"

/*
 * The gate DoCopy() puts on a file on the server: COPY FROM one needs the
 * privileges of pg_read_server_files, COPY TO one those of
 * pg_write_server_files. Raises for a role without them.
 */
extern void
chdb_check_server_file_privileges(bool is_from);

/*
 * Opens and locks the relation `copy` names, with the privilege checks
 * DoCopy() applies to a normal COPY: INSERT or SELECT on the relation or on
 * each copied column, then row-level security. Errors out unless the current
 * user may copy the relation. Fills `ctx` with the locked relation, the
 * columns it checked and the range table it checked them under; the caller
 * closes the relation.
 */
extern void
chdb_open_copy_relation(CopyStmt* copy, chdbCopyContext* ctx);

#endif /* CHDB_RELATION_H */
