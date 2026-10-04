/*
 * libchdb's storage callbacks, each a page request to the supervisor
 * (pagecall.c), registered per storage by pageregistry.c. The engine's
 * threads call these concurrently while the thread inside libchdb is
 * blocked. Reads and appends larger than a frame may carry go in several.
 * See pagestore.h and chdb.h's contract for the callbacks.
 */

#include <stdlib.h>
#include <string.h>

#include "chdb.h"

#include "../pagestore/protocol.h"
#include "pagecall.h"
#include "pagestore.h"
#include "readonly.h"

/* The ud of the callbacks: the storage they serve. */
#define NAME(ud) (((pageStorage*)(ud))->name)

/* A pending write: the number the supervisor gave it. */
typedef struct Handle {
    uint64_t id;
} Handle;

/* ---- the callbacks ---- */

static int
cb_exists(void* ud, const char* key, int* out) {
    pageBody b  = { 0 };
    pageReply r = { 0 };

    pagebody_str(&b, NAME(ud));
    pagebody_str(&b, key);
    if (pagecall_body(CHDB_PAGE_EXISTS, &b, NULL, 0, &r)) {
        return 1;
    }
    *out = r.len >= 1 && r.body[0] != 0;
    free(r.body);

    return 0;
}

static int
cb_metadata(void* ud, const char* key, int* found, uint64_t* size, int64_t* mtime) {
    pageBody b  = { 0 };
    pageReply r = { 0 };
    uint8_t f   = 0;

    pagebody_str(&b, NAME(ud));
    pagebody_str(&b, key);
    if (pagecall_body(CHDB_PAGE_METADATA, &b, NULL, 0, &r)) {
        return 1;
    }

    chdbSetupCursor c = { .at = r.body, .end = r.body + r.len };

    chdb_setup_take(&c, &f, sizeof(f));
    *found = f;
    if (f && !(chdb_setup_take(&c, size, sizeof(*size)) &&
               chdb_setup_take(&c, mtime, sizeof(*mtime)))) {
        free(r.body);
        return pagecall_fail("malformed metadata reply");
    }
    free(r.body);

    return 0;
}

static int
cb_read(
    void* ud,
    const char* key,
    uint64_t offset,
    void* buf,
    size_t len,
    size_t* out
) {
    size_t got = 0;

    while (got < len) {
        uint32_t want =
            (uint32_t)(len - got < CHDB_PAGE_DATA_MAX ? len - got : CHDB_PAGE_DATA_MAX);
        pageBody b  = { 0 };
        pageReply r = { .body = (char*)buf + got, .cap = want, .fixed = true };

        pagebody_str(&b, NAME(ud));
        pagebody_str(&b, key);
        pagebody_u64(&b, offset + got);
        pagebody_put(&b, &want, sizeof(want));
        if (pagecall_body(CHDB_PAGE_READ, &b, NULL, 0, &r)) {
            return 1;
        }
        got += r.len;
        if (r.len < want) {
            break; /* the end of the blob */
        }
    }
    *out = got;

    return 0;
}

static int
cb_write_begin(void* ud, const char* key, void** handle) {
    pageBody b  = { 0 };
    pageReply r = { 0 };
    Handle* h   = malloc(sizeof(*h));

    if (!h) {
        return pagecall_fail("out of memory");
    }
    pagebody_str(&b, NAME(ud));
    pagebody_str(&b, key);
    if (pagecall_body(CHDB_PAGE_WRITE_BEGIN, &b, NULL, 0, &r)) {
        free(h);
        return 1;
    }

    chdbSetupCursor c = { .at = r.body, .end = r.body + r.len };
    bool ok           = chdb_setup_take(&c, &h->id, sizeof(h->id));

    free(r.body);
    if (!ok) {
        free(h);
        return pagecall_fail("malformed write_begin reply");
    }
    *handle = h;

    return 0;
}

static int
cb_write_append(void* ud, void* handle, const void* buf, size_t len) {
    (void)ud;
    for (size_t at = 0; at < len;) {
        size_t n    = len - at < CHDB_PAGE_DATA_MAX ? len - at : CHDB_PAGE_DATA_MAX;
        pageBody b  = { 0 };
        pageReply r = { 0 };

        pagebody_u64(&b, ((Handle*)handle)->id);
        if (pagecall_body(CHDB_PAGE_WRITE_APPEND, &b, (const char*)buf + at, n, &r)) {
            return 1;
        }
        free(r.body);
        at += n;
    }

    return 0;
}

/* Commit and abort both release the handle, whatever the supervisor says. */
static int
end_write(uint32_t op, void* handle) {
    pageBody b  = { 0 };
    pageReply r = { 0 };

    pagebody_u64(&b, ((Handle*)handle)->id);
    free(handle);

    int rc = pagecall_body(op, &b, NULL, 0, &r);

    free(r.body);

    return rc;
}

static int
cb_write_commit(void* ud, void* handle) {
    (void)ud;
    return end_write(CHDB_PAGE_WRITE_COMMIT, handle);
}

static int
cb_write_abort(void* ud, void* handle) {
    (void)ud;
    return end_write(CHDB_PAGE_WRITE_ABORT, handle);
}

static int
cb_remove(void* ud, const char* key) {
    pageBody b  = { 0 };
    pageReply r = { 0 };

    pagebody_str(&b, NAME(ud));
    pagebody_str(&b, key);

    int rc = pagecall_body(CHDB_PAGE_REMOVE, &b, NULL, 0, &r);

    free(r.body);

    return rc;
}

static int
cb_copy(void* ud, const char* from, const char* to) {
    pageBody b  = { 0 };
    pageReply r = { 0 };

    pagebody_str(&b, NAME(ud));
    pagebody_str(&b, from);
    pagebody_str(&b, to);

    int rc = pagecall_body(CHDB_PAGE_COPY, &b, NULL, 0, &r);

    free(r.body);

    return rc;
}

static int
cb_list(
    void* ud,
    const char* prefix,
    chdb_object_storage_list_sink sink,
    void* sink_ud
) {
    pageBody b  = { 0 };
    pageReply r = { 0 };
    chdbSetupStr key;
    uint64_t size;
    int64_t mtime;

    pagebody_str(&b, NAME(ud));
    pagebody_str(&b, prefix);
    if (pagecall_body(CHDB_PAGE_LIST, &b, NULL, 0, &r)) {
        return 1;
    }

    chdbSetupCursor c = { .at = r.body, .end = r.body + r.len };

    while (chdb_setup_take_str(&c, &key) && chdb_setup_take(&c, &size, sizeof(size)) &&
           chdb_setup_take(&c, &mtime, sizeof(mtime))) {
        char* name = strndup(key.data, key.len);
        int stop   = !name || sink(sink_ud, name, size, mtime);

        free(name);
        if (stop) {
            break;
        }
    }
    free(r.body);

    return 0;
}

static const char*
cb_last_error(void* ud) {
    (void)ud;
    return pagecall_last_error();
}

void
pagestore_callbacks(chdb_object_storage_callbacks* cb, pageStorage* storage) {
    *cb = (chdb_object_storage_callbacks){
        .struct_size  = sizeof(*cb),
        .ud           = storage,
        .exists       = cb_exists,
        .metadata     = cb_metadata,
        .read         = cb_read,
        .write_begin  = cb_write_begin,
        .write_append = cb_write_append,
        .write_commit = cb_write_commit,
        .write_abort  = cb_write_abort,
        .remove       = cb_remove,
        .list         = cb_list,
        .copy         = cb_copy,
        .last_error   = cb_last_error,
    };
    readonly_callbacks(cb);
}
