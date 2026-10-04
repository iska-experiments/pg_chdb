/*
 * The blob store's resource owner, and the public calls of pagestore.h that
 * run under it outside a page request: noting and forgetting relations,
 * listing for the worker's own use, and the shutdown. See owner.h.
 */

#include "postgres.h"

#include "storage/bufmgr.h"
#include "storage/lwlock.h"

#include "owner.h"
#include "pagestore.h"
#include "routes.h"

/* Owns what the store pins and locks while a call runs. */
static ResourceOwner owner;

void
chdb_pagestore_owner_init(void) {
    owner = ResourceOwnerCreate(NULL, "chdb_search page store");
}

ResourceOwner
chdb_pagestore_enter(void) {
    ResourceOwner saved  = CurrentResourceOwner;
    CurrentResourceOwner = owner;
    return saved;
}

void
chdb_pagestore_leave(ResourceOwner saved, bool failed) {
    if (failed) {
        LWLockReleaseAll();
        UnlockBuffers();
        ResourceOwnerRelease(owner, RESOURCE_RELEASE_BEFORE_LOCKS, false, true);
    }
    CurrentResourceOwner = saved;
}

/* ---- the calls made outside a request ---- */

void
chdb_pagestore_note(Oid index, RelFileLocator loc) {
    ResourceOwner saved = chdb_pagestore_enter();

    PG_TRY();
    { chdb_routes_note(index, loc); }
    PG_CATCH();
    {
        chdb_pagestore_leave(saved, true);
        PG_RE_THROW();
    }
    PG_END_TRY();
    chdb_pagestore_leave(saved, false);
}

void
chdb_pagestore_forget(Oid index) {
    chdb_routes_forget(index);
}

void
chdb_pagestore_list(
    const char* storage,
    const char* prefix,
    chdbBlobListSink sink,
    void* ud
) {
    ResourceOwner saved = chdb_pagestore_enter();

    PG_TRY();
    { chdb_blob_pagestore.list(storage, prefix, sink, ud); }
    PG_CATCH();
    {
        chdb_pagestore_leave(saved, true);
        PG_RE_THROW();
    }
    PG_END_TRY();
    chdb_pagestore_leave(saved, false);
}

void
chdb_pagestore_shutdown(void) {
    ResourceOwner saved = chdb_pagestore_enter();

    PG_TRY();
    { chdb_routes_clean(); }
    PG_CATCH();
    {
        /* A relation gone from under a worker on its way out is no loss. */
        FlushErrorState();
    }
    PG_END_TRY();
    chdb_pagestore_leave(saved, true);
}
