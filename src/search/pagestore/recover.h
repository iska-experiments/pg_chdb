#ifndef CHDB_SEARCH_PAGESTORE_RECOVER_H
#define CHDB_SEARCH_PAGESTORE_RECOVER_H

/* Reclaims the pages a crash left unreachable (recover.c). */

#include "postgres.h"

#include "pages.h"

/* For an open relation with no write pending; clears CHDB_META_DIRTY. */
extern void
chdb_pages_recover(ChdbPages* p);

#endif /* CHDB_SEARCH_PAGESTORE_RECOVER_H */
