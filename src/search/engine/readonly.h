#ifndef CHDB_SEARCH_ENGINE_READONLY_H
#define CHDB_SEARCH_ENGINE_READONLY_H

/*
 * The engine on a server in recovery (readonly.c): the supervisor cannot
 * write the pages the blobs live in, so every callback that would write
 * refuses with a clear error, and the session runs no merges. The
 * supervisor attaches the tables read-only as well (../attach.c), and
 * promotion restarts the engine without the flag.
 */

#include <stdbool.h>

#include "chdb.h"

/* Before the callbacks are built: whether this engine runs read-only. */
extern void
readonly_set(bool readonly);

extern bool
readonly_is(void);

/* Swaps the writing callbacks of `cb` for refusals when read-only. */
extern void
readonly_callbacks(chdb_object_storage_callbacks* cb);

/* After the session opens: stops the background work that would write. */
extern bool
readonly_session(void);

#endif /* CHDB_SEARCH_ENGINE_READONLY_H */
