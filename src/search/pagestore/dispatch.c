/*
 * Answering one page request of protocol.h: read whole from the page channel,
 * run against the blob store of store.h, its reply written back. A failure
 * of the store becomes an error reply, the engine raising it as libchdb's
 * CALLBACK_OBJECT_STORAGE_ERROR; a channel that breaks raises to the caller.
 * The store's write handles are kept here by number, since the engine may
 * only name what it can repeat, and dropped together when the engine goes.
 */

#include "postgres.h"

#include "lib/stringinfo.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "../protocol.h"
#include "pagestore.h"
#include "protocol.h"
#include "store.h"

static const chdbBlobStore* store = &chdb_blob_dirstore;

/* The pending writes, by the number the engine was given. */
typedef struct HandleEntry {
    uint64 id;
    void* handle;
} HandleEntry;

static HTAB* handles;
static uint64 next_handle = 1;

/* Reset after every request; the store's handles live in TopMemoryContext. */
static MemoryContext request_cxt;

void
chdb_pagestore_init(void) {
    HASHCTL ctl = {
        .keysize   = sizeof(uint64),
        .entrysize = sizeof(HandleEntry),
        .hcxt      = TopMemoryContext,
    };

    handles = hash_create(
        "chdb_search blob handles", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT
    );
    request_cxt = AllocSetContextCreate(
        TopMemoryContext, "chdb_search page request", ALLOCSET_DEFAULT_SIZES
    );
    store->init();
}

void
chdb_pagestore_list(
    const char* storage,
    const char* prefix,
    chdbBlobListSink sink,
    void* ud
) {
    store->list(storage, prefix, sink, ud);
}

void
chdb_pagestore_remove_storage(Oid indexoid) {
    store->remove_storage(psprintf(CHDB_STORE_STORAGE_FMT, indexoid));
}

/* ---- the body of a request ---- */

static void
malformed(void) {
    ereport(
        ERROR,
        errcode(ERRCODE_PROTOCOL_VIOLATION),
        errmsg("chdb_search: malformed page request")
    );
}

static char*
take_str(chdbSetupCursor* cur) {
    chdbSetupStr s;

    if (!chdb_setup_take_str(cur, &s)) {
        malformed();
    }
    return pnstrdup(s.data, s.len);
}

static uint64
take_u64(chdbSetupCursor* cur) {
    uint64 v;

    if (!chdb_setup_take(cur, &v, sizeof(v))) {
        malformed();
    }
    return v;
}

static uint32
take_u32(chdbSetupCursor* cur) {
    uint32 v;

    if (!chdb_setup_take(cur, &v, sizeof(v))) {
        malformed();
    }
    return v;
}

/* The store's handle behind the number read, forgotten here if `release`. */
static void*
take_handle(chdbSetupCursor* cur, bool release) {
    uint64 id = take_u64(cur);
    bool found;
    HandleEntry* e =
        hash_search(handles, &id, release ? HASH_REMOVE : HASH_FIND, &found);

    if (!found) {
        ereport(
            ERROR,
            errcode(ERRCODE_PROTOCOL_VIOLATION),
            errmsg("chdb_search: unknown blob handle " UINT64_FORMAT, id)
        );
    }
    return e->handle;
}

/* ---- the body of a reply ---- */

static void
put_str(StringInfo out, const char* s) {
    uint32 len = (uint32)strlen(s);

    appendBinaryStringInfo(out, (char*)&len, sizeof(len));
    appendBinaryStringInfo(out, s, len);
}

static void
name_sink(void* ud, const char* name) {
    put_str(ud, name);
}

static void
list_sink(void* ud, const char* key, uint64 size, int64 mtime) {
    put_str(ud, key);
    appendBinaryStringInfo(ud, (char*)&size, sizeof(size));
    appendBinaryStringInfo(ud, (char*)&mtime, sizeof(mtime));
}

/* ---- the operations ---- */

static void
answer(uint32 op, chdbSetupCursor* cur, StringInfo out) {
    switch (op) {
    case CHDB_PAGE_STORAGES:
        store->storages(name_sink, out);
        break;
    case CHDB_PAGE_EXISTS: {
        char* storage = take_str(cur);
        uint8 found   = store->exists(storage, take_str(cur));

        appendBinaryStringInfo(out, (char*)&found, sizeof(found));
        break;
    }
    case CHDB_PAGE_METADATA: {
        char* storage = take_str(cur);
        uint64 size   = 0;
        int64 mtime   = 0;
        uint8 found   = store->metadata(storage, take_str(cur), &size, &mtime);

        appendBinaryStringInfo(out, (char*)&found, sizeof(found));
        if (found) {
            appendBinaryStringInfo(out, (char*)&size, sizeof(size));
            appendBinaryStringInfo(out, (char*)&mtime, sizeof(mtime));
        }
        break;
    }
    case CHDB_PAGE_READ: {
        char* storage = take_str(cur);
        char* key     = take_str(cur);
        uint64 offset = take_u64(cur);
        uint32 want   = take_u32(cur);

        if (want > CHDB_PAGE_DATA_MAX) {
            malformed();
        }
        enlargeStringInfo(out, (int)want);
        out->len += (int)store->read(storage, key, offset, out->data + out->len, want);
        break;
    }
    case CHDB_PAGE_WRITE_BEGIN: {
        char* storage = take_str(cur);
        void* handle  = store->write_begin(storage, take_str(cur));
        uint64 id     = next_handle++;
        bool found;

        ((HandleEntry*)hash_search(handles, &id, HASH_ENTER, &found))->handle = handle;
        appendBinaryStringInfo(out, (char*)&id, sizeof(id));
        break;
    }
    case CHDB_PAGE_WRITE_APPEND: {
        void* handle = take_handle(cur, false);

        store->write_append(handle, cur->at, (size_t)(cur->end - cur->at));
        return; /* the data is the rest of the body */
    }
    case CHDB_PAGE_WRITE_COMMIT:
        store->write_commit(take_handle(cur, true));
        break;
    case CHDB_PAGE_WRITE_ABORT:
        store->write_abort(take_handle(cur, true));
        break;
    case CHDB_PAGE_REMOVE: {
        char* storage = take_str(cur);

        store->remove(storage, take_str(cur));
        break;
    }
    case CHDB_PAGE_LIST: {
        char* storage = take_str(cur);

        store->list(storage, take_str(cur), list_sink, out);
        break;
    }
    case CHDB_PAGE_COPY: {
        char* storage = take_str(cur);
        char* from    = take_str(cur);

        store->copy(storage, from, take_str(cur));
        break;
    }
    default:
        ereport(
            ERROR,
            errcode(ERRCODE_PROTOCOL_VIOLATION),
            errmsg("chdb_search: unknown page request %u", op)
        );
    }
    if (cur->at != cur->end) {
        malformed();
    }
}

void
chdb_pagestore_serve(chdbChannel* page) {
    MemoryContext old = MemoryContextSwitchTo(request_cxt);
    chdbPageRequest req;
    chdbPageReply rep = { 0 };
    StringInfoData body;
    StringInfoData out;

    chdb_channel_recv_exact(page, &req, sizeof(req));
    if (req.len > CHDB_PAGE_BODY_MAX) {
        /* The framing cannot be followed past this. */
        chdb_channel_close(page);
        ereport(
            ERROR,
            errcode(ERRCODE_PROTOCOL_VIOLATION),
            errmsg("chdb_search: the chDB engine sent a malformed page request"),
            errdetail("Its body would be %u bytes long.", req.len)
        );
    }
    initStringInfo(&body);
    enlargeStringInfo(&body, (int)req.len);
    chdb_channel_recv_exact(page, body.data, req.len);
    body.len = (int)req.len;

    chdbSetupCursor cur = { .at = body.data, .end = body.data + body.len };

    initStringInfo(&out);
    rep.id     = req.id;
    rep.status = CHDB_PAGE_OK;
    PG_TRY();
    { answer(req.op, &cur, &out); }
    PG_CATCH();
    {
        MemoryContextSwitchTo(request_cxt);

        ErrorData* e = CopyErrorData();

        FlushErrorState();
        elog(DEBUG1, "chdb_search: page request %u failed: %s", req.op, e->message);
        resetStringInfo(&out);
        appendStringInfoString(&out, e->message);
        rep.status = CHDB_PAGE_ERROR;
    }
    PG_END_TRY();
    if (out.len > CHDB_PAGE_BODY_MAX) {
        resetStringInfo(&out);
        appendStringInfoString(&out, "the reply would exceed the protocol's bound");
        rep.status = CHDB_PAGE_ERROR;
    }
    rep.len = (uint32)out.len;
    chdb_channel_send_exact(page, &rep, sizeof(rep));
    chdb_channel_send_exact(page, out.data, out.len);

    MemoryContextSwitchTo(old);
    MemoryContextReset(request_cxt);
}

void
chdb_pagestore_engine_gone(void) {
    HASH_SEQ_STATUS seq;
    HandleEntry* e;
    long n = handles ? hash_get_num_entries(handles) : 0;

    if (n == 0) {
        return;
    }
    hash_seq_init(&seq, handles);
    while ((e = hash_seq_search(&seq)) != NULL) {
        store->write_abort(e->handle);
        hash_search(handles, &e->id, HASH_REMOVE, NULL);
    }
    ereport(
        LOG,
        errmsg("chdb_search: dropped %ld pending blob writes of the chDB engine", n)
    );
}
