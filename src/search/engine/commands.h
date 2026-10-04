#ifndef CHDB_SEARCH_ENGINE_COMMANDS_H
#define CHDB_SEARCH_ENGINE_COMMANDS_H

/*
 * The commands of ../protocol.h run against the session. Each returns true
 * while the supervisor is still in step.
 */

#include "io.h"

extern bool
command_exec(int fd, const request* req);

extern bool
command_drop(int fd, const request* req);

extern bool
command_select(int fd, const request* req);

extern bool
command_insert(int fd, const request* req);

#endif /* CHDB_SEARCH_ENGINE_COMMANDS_H */
