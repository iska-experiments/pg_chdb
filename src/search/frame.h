#ifndef CHDB_SEARCH_FRAME_H
#define CHDB_SEARCH_FRAME_H

/*
 * The request frame of protocol.h, built the same way by the backend client
 * for the worker and by the worker for the queries of its own startup sweep.
 */

#include "postgres.h"

#include "lib/stringinfo.h"

#include "../setup.h"

/*
 * Appends a whole request to `buf`: `cmd` against index `index`, with the
 * chDB settings of the GUCs, carrying `sql` and no parameters. Raises if
 * the frame would exceed what the other side takes.
 */
extern void
chdb_search_frame_request(StringInfo buf, chdbCmdType cmd, Oid index, const char* sql);

#endif /* CHDB_SEARCH_FRAME_H */
