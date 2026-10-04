#ifndef CHDB_SEARCH_COMMANDS_H
#define CHDB_SEARCH_COMMANDS_H

/*
 * The commands of protocol.h run against the chDB session. Each returns true
 * while the connection is still in step with the client.
 */

#include "postgres.h"

#include "request.h"

extern bool
command_exec(int fd, const chdbSearchRequest* req);

extern bool
command_drop(int fd, const chdbSearchRequest* req);

extern bool
command_select(int fd, const chdbSearchRequest* req);

extern bool
command_insert(int fd, const chdbSearchRequest* req);

#endif /* CHDB_SEARCH_COMMANDS_H */
