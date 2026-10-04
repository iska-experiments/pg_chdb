#ifndef CHDB_SEARCH_RELAY_H
#define CHDB_SEARCH_RELAY_H

/*
 * Forwarding one request and its reply between a client and the engine. The
 * bytes pass through as they are, so the client's protocol is the engine's.
 */

#include "postgres.h"

#include "lib/stringinfo.h"

#include "../channel.h"

/*
 * Runs `raw`, a whole request of `cmd`, through the engine for `client`.
 * Returns NULL when the reply, status frame included, reached the client.
 * Otherwise the engine failed and the result says how; `*data_open` is then
 * true if the client is owed the end of a data section as well as a status.
 * A client that fails raises.
 */
extern char*
relay_request(chdbChannel* client, const StringInfoData* raw, int cmd, bool* data_open);

/* Whether the engine's status to the request just relayed was CHDB_STATUS_OK. */
extern bool
relay_succeeded(void);

/*
 * The client of the request under way is gone. Reads what the engine still
 * owes it, so that the engine is in step for the next request, except an
 * insert's chunks, which only the client could send: that engine is put
 * down. NULL, or the engine's death.
 */
extern char*
relay_abandon(void);

#endif /* CHDB_SEARCH_RELAY_H */
