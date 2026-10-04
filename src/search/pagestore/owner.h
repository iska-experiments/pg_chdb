#ifndef CHDB_SEARCH_PAGESTORE_OWNER_H
#define CHDB_SEARCH_PAGESTORE_OWNER_H

/*
 * The resource owner the blob store runs under (owner.c). The worker serves
 * page requests outside any transaction, so the buffer pins and locks the
 * pages take have no owner to fall back on: every call into the store is
 * bracketed by these, and a call that raised has what it left pinned or
 * locked released before the error goes on.
 */

#include "postgres.h"

#include "utils/resowner.h"

/* Once per worker. */
extern void
chdb_pagestore_owner_init(void);

/* Makes the store's owner current and returns the one to restore. */
extern ResourceOwner
chdb_pagestore_enter(void);

/* Restores the owner; with `failed`, releases what the call left behind first. */
extern void
chdb_pagestore_leave(ResourceOwner saved, bool failed);

#endif /* CHDB_SEARCH_PAGESTORE_OWNER_H */
