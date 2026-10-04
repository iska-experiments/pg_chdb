/*
 * Forwarding requests, data and status frames between a client and the
 * engine. See relay.h and ../search/protocol.h.
 */

#include "postgres.h"

#include "miscadmin.h"
#include "utils/memutils.h"

#include "engine_proc.h"
#include "protocol.h"
#include "relay.h"

/* One buffer for every chunk, kept across requests so a big select allocates once. */
static char* scratch_buf;
static size_t scratch_cap;

static char*
scratch(size_t len) {
    if (len > scratch_cap) {
        if (scratch_buf) {
            pfree(scratch_buf);
        }
        scratch_buf = MemoryContextAlloc(TopMemoryContext, len);
        scratch_cap = len;
    }

    return scratch_buf;
}

/* The engine broke the framing; it cannot be trusted with another request. */
static char*
engine_garbled(void) {
    pfree(engine_death());

    return pstrdup("the chDB engine sent a malformed reply");
}

/* Forwards the status frame. NULL, or the engine's death. */
static char*
relay_status(chdbChannel* client) {
    uint8_t status;
    uint32_t len;

    if (!engine_recv(&status, sizeof(status)) || !engine_recv(&len, sizeof(len))) {
        return engine_death();
    }
    if (len > CHDB_SEARCH_CHUNK_MAX) {
        return engine_garbled();
    }

    char* text = scratch(len);

    if (len && !engine_recv(text, len)) {
        return engine_death();
    }
    chdb_channel_send_exact(client, &status, sizeof(status));
    chdb_channel_send_exact(client, &len, sizeof(len));
    chdb_channel_send_exact(client, text, len);

    return NULL;
}

/*
 * Forwards a select's chunks, each whole, so the client never sees half of
 * one. Ends with the zero chunk, or with the engine's death before it.
 */
static char*
relay_out(chdbChannel* client, bool* data_open) {
    *data_open = true;
    for (;;) {
        uint32_t len;

        if (!engine_recv(&len, sizeof(len))) {
            return engine_death();
        }
        if (len > CHDB_SEARCH_CHUNK_MAX) {
            return engine_garbled();
        }

        char* chunk = scratch(len);

        if (len && !engine_recv(chunk, len)) {
            return engine_death();
        }
        chdb_channel_send_exact(client, &len, sizeof(len));
        chdb_channel_send_exact(client, chunk, len);
        if (len == 0) {
            *data_open = false;
            return NULL;
        }
    }
}

/*
 * Forwards an insert's chunks to the engine. If the engine dies partway the
 * client's chunks are still read to the end, as the status must follow them.
 * `dead` is the engine's death when it was known before the first chunk.
 */
static char*
relay_in(chdbChannel* client, char* dead) {
    for (;;) {
        uint32_t len;

        chdb_channel_recv_exact(client, &len, sizeof(len));
        if (len > CHDB_SEARCH_CHUNK_MAX) {
            /* The client's framing is gone; the engine's insert with it. */
            pfree(engine_death());
            ereport(ERROR, errmsg("chdb_search: client sent a bad chunk"));
        }

        char* chunk = scratch(len);

        chdb_channel_recv_exact(client, chunk, len);
        if (!dead && (!engine_send(&len, sizeof(len)) || !engine_send(chunk, len))) {
            dead = engine_death();
        }
        if (len == 0) {
            return dead ? dead : relay_status(client);
        }
    }
}

char*
relay_request(
    chdbChannel* client,
    const StringInfoData* raw,
    int cmd,
    bool* data_open
) {
    char* dead = engine_ensure(MyDatabaseId);

    /* A select's client waits for the end of the data before the status. */
    *data_open = cmd == CHDB_CMD_SELECT;
    if (!dead && !engine_send(raw->data, raw->len)) {
        dead = engine_death();
    }
    if (dead) {

        /* An insert's chunks are on their way regardless. */
        return cmd == CHDB_CMD_INSERT ? relay_in(client, dead) : dead;
    }

    switch (cmd) {
    case CHDB_CMD_SELECT: {
        char* err = relay_out(client, data_open);

        return err ? err : relay_status(client);
    }
    case CHDB_CMD_INSERT:
        return relay_in(client, NULL);
    default:
        return relay_status(client);
    }
}
