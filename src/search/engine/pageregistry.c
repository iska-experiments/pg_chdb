/*
 * The storages the engine serves: one per index, named CHDB_STORE_STORAGE_FMT,
 * registered with libchdb before a table is made on it, and all of those the
 * supervisor holds before the store is opened, since a persisted table is
 * attached to its storage by name. See pagestore.h.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chdb.h"

#include "../pagestore/protocol.h"
#include "pagecall.h"
#include "pagestore.h"

typedef chdb_state (*registerFn)(const char*, const chdb_object_storage_callbacks*);

static registerFn register_storage;
static pageStorage** registered;
static size_t nregistered;

bool
pagestore_start(int fd) {
    register_storage = (registerFn)dlsym(RTLD_DEFAULT, "chdb_register_object_storage");
    if (!register_storage) {
        fprintf(
            stderr,
            "chdb_search_engine: libchdb %s has no callback object storage\n"
            "chdb_search_engine: the server's library path needs a libchdb.so with "
            "chdb_register_object_storage\n",
            chdb_version()
        );
        return false;
    }

    return pagecall_start(fd);
}

bool
pagestore_register(const char* name) {
    for (size_t i = 0; i < nregistered; i++) {
        if (strcmp(registered[i]->name, name) == 0) {
            return true;
        }
    }

    if (strlen(name) >= CHDB_PAGE_STORAGE_MAX) {
        fprintf(stderr, "chdb_search_engine: storage name \"%s\" is too long\n", name);
        return false;
    }

    pageStorage* s = calloc(1, sizeof(*s));
    pageStorage** more =
        s ? realloc(registered, (nregistered + 1) * sizeof(*more)) : NULL;

    if (!more) {
        fprintf(stderr, "chdb_search_engine: out of memory registering a storage\n");
        free(s);
        return false;
    }
    registered = more;
    strcpy(s->name, name);

    chdb_object_storage_callbacks cb;

    pagestore_callbacks(&cb, s);
    if (register_storage(name, &cb) != CHDBSuccess) {
        fprintf(stderr, "chdb_search_engine: libchdb refused storage \"%s\"\n", name);
        free(s);
        return false;
    }
    registered[nregistered++] = s;

    return true;
}

bool
pagestore_bootstrap(void) {
    pageBody b  = { 0 };
    pageReply r = { 0 };
    chdbSetupStr name;
    bool ok = true;

    if (pagecall_body(CHDB_PAGE_STORAGES, &b, NULL, 0, &r)) {
        fprintf(
            stderr,
            "chdb_search_engine: could not list the storages: %s\n",
            pagecall_last_error()
        );
        return false;
    }

    chdbSetupCursor c = { .at = r.body, .end = r.body + r.len };

    while (ok && chdb_setup_take_str(&c, &name)) {
        char* n = strndup(name.data, name.len);

        ok = n && pagestore_register(n);
        free(n);
    }
    free(r.body);

    return ok;
}
