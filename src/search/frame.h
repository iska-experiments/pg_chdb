#ifndef CHDB_SEARCH_FRAME_H
#define CHDB_SEARCH_FRAME_H

/*
 * The frames of protocol.h as the two sides that make requests handle them:
 * the backend client for the worker, and the worker for the requests of its
 * own startup sweep. The request frame is built here and the status frame
 * read here, so the layout is walked in one place on each side.
 */

#include "postgres.h"

#include "lib/stringinfo.h"

#include "../channel.h"
#include "../setup.h"

/*
 * Appends a whole request to `buf`: its byte count, then `cmd` against index
 * `index` with the chDB settings of the GUCs, carrying `sql` and no
 * parameters. Raises if the frame would exceed what the other side takes.
 */
extern void
chdb_search_frame_request(StringInfo buf, chdbCmdType cmd, Oid index, const char* sql);

/*
 * Reads the status frame that ends every request from `ch`: the status byte
 * into `status`, and its text, returned palloc'd and terminated. A text length
 * past CHDB_CHUNK_MAX cannot be followed, so the channel is closed and the
 * error raised names `peer`, with `query` for context.
 */
extern char*
chdb_search_frame_status(
    chdbChannel* ch,
    const char* peer,
    const char* query,
    uint8_t* status
);

/* Reads a chunked data stream to its end, for a reader that stopped early. */
extern void
chdb_search_frame_skip_data(chdbChannel* ch);

#endif /* CHDB_SEARCH_FRAME_H */
