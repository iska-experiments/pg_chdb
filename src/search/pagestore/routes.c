/*
 * The relations behind the engine's storages, by index and generation. See
 * routes.h for why the worker is told rather than asking the catalog.
 */

#include "postgres.h"

#include <ctype.h>

#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "../protocol.h"
#include "pages.h"
#include "protocol.h"
#include "recover.h"
#include "routes.h"

typedef struct RouteKey {
    Oid index;
    uint64 generation;
} RouteKey;

typedef struct Route {
    RouteKey key;
    RelFileLocator loc;
    bool dirtied; /* this worker took pages in it, so its flag is to clear */
} Route;

static HTAB* routes;

void
chdb_routes_init(void) {
    HASHCTL ctl = {
        .keysize   = sizeof(RouteKey),
        .entrysize = sizeof(Route),
        .hcxt      = TopMemoryContext,
    };

    routes = hash_create(
        "chdb_search blob routes", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT
    );
}

void
chdb_routes_note(Oid index, RelFileLocator loc) {
    ChdbPages p;
    bool found;

    if (!OidIsValid(index) || !RelFileNumberIsValid(loc.relNumber) ||
        !chdb_pages_open(&p, loc, NULL)) {
        return; /* nothing of ours there yet, or no longer */
    }

    RouteKey key = { .index = index, .generation = p.meta.generation };
    Route* r     = hash_search(routes, &key, HASH_ENTER, &found);

    if (!found) {
        r->dirtied = false;
        /* First sight of a relation a worker died writing: before any blob. */
        if (p.meta.flags & CHDB_META_DIRTY) {
            chdb_pages_recover(&p);
        }
    }
    r->loc = loc;
}

void
chdb_routes_forget(Oid index) {
    HASH_SEQ_STATUS seq;
    Route* r;

    hash_seq_init(&seq, routes);
    while ((r = hash_seq_search(&seq)) != NULL) {
        if (r->key.index == index) {
            hash_search(routes, &r->key, HASH_REMOVE, NULL);
        }
    }
}

static void
malformed(const char* what, const char* s) {
    ereport(
        ERROR,
        errcode(ERRCODE_INVALID_PARAMETER_VALUE),
        errmsg("chdb_search: invalid blob %s \"%.80s\"", what, s)
    );
}

/* pg_<oid> */
static Oid
parse_storage(const char* storage) {
    char* end;
    unsigned long v;

    if (strncmp(storage, "pg_", 3) != 0 || !isdigit((unsigned char)storage[3])) {
        malformed("storage name", storage);
    }
    v = strtoul(storage + 3, &end, 10);
    if (*end != '\0' || v == 0 || v != (Oid)v) {
        malformed("storage name", storage);
    }
    return (Oid)v;
}

/* g<generation>/... */
static uint64
parse_key(const char* key) {
    char* end;
    uint64 v;

    if (key[0] != 'g' || !isdigit((unsigned char)key[1])) {
        malformed("key", key);
    }
    v = strtoull(key + 1, &end, 10);
    if (*end != '/') {
        malformed("key", key);
    }
    return v;
}

bool
chdb_routes_find(const char* storage, const char* key, RelFileLocator* loc) {
    RouteKey k = { .index = parse_storage(storage), .generation = parse_key(key) };
    Route* r   = hash_search(routes, &k, HASH_FIND, NULL);

    if (r) {
        *loc = r->loc;
        /* Pages may be taken in it now; the flag that sets is ours to clear. */
        r->dirtied = true;
    }
    return r != NULL;
}

void
chdb_routes_storages(chdbBlobNameSink sink, void* ud) {
    HASH_SEQ_STATUS seq;
    Route* r;
    List* seen = NIL;

    hash_seq_init(&seq, routes);
    while ((r = hash_seq_search(&seq)) != NULL) {
        if (!list_member_oid(seen, r->key.index)) {
            seen = lappend_oid(seen, r->key.index);
            sink(ud, psprintf(CHDB_STORE_STORAGE_FMT, r->key.index));
        }
    }
    list_free(seen);
}

void
chdb_routes_clean(void) {
    HASH_SEQ_STATUS seq;
    Route* r;

    hash_seq_init(&seq, routes);
    while ((r = hash_seq_search(&seq)) != NULL) {
        ChdbPages p;

        if (r->dirtied && chdb_pages_open(&p, r->loc, NULL)) {
            chdb_pages_clean(&p);
        }
    }
}
