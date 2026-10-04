/*
 * Forwarding requests, data and status frames between a client and the
 * engine. See relay.h and protocol.h.
 */

#include "postgres.h"

#include "miscadmin.h"
#include "utils/memutils.h"

#include "engine_proc.h"
#include "protocol.h"
#include "relay.h"

/*
 * What the engine still owes the relay: nothing; the chunks of a select and
 * its status; its status alone; or, for an insert, the client's chunks it
 * waits for. A client that goes away mid-request leaves the engine at that
 * point, and relay_abandon reads what is owed so that the engine is in step
 * for the next request.
 */
typedef enum RelayOwed {
    OWED_NOTHING,
    OWED_DATA,
    OWED_STATUS,
    OWED_CLIENT_CHUNKS,
} RelayOwed;

static RelayOwed owed;

/* The status byte of the last reply read whole, for a request whose outcome matters. */
static uint8_t last_status;

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

/*
 * Forwards the status frame, or with no client reads it and drops it. NULL,
 * or the engine's death. Each frame is read whole before any of it is sent,
 * so a client that fails leaves the engine's stream at a frame boundary.
 */
static char*
relay_status(chdbChannel* client) {
    uint8_t status;
    uint32_t len;

    owed = OWED_STATUS;
    if (!engine_recv(&status, sizeof(status)) || !engine_recv(&len, sizeof(len))) {
        return engine_death();
    }
    if (len > CHDB_CHUNK_MAX) {
        return engine_garbled();
    }

    char* text = scratch(len);

    if (len && !engine_recv(text, len)) {
        return engine_death();
    }
    owed        = OWED_NOTHING;
    last_status = status;
    if (client) {
        chdb_channel_send_exact(client, &status, sizeof(status));
        chdb_channel_send_exact(client, &len, sizeof(len));
        chdb_channel_send_exact(client, text, len);
    }

    return NULL;
}

/*
 * Forwards a select's chunks, each whole, so the client never sees half of
 * one, or with no client reads them and drops them. Ends with the zero
 * chunk, or with the engine's death before it.
 */
static char*
relay_out(chdbChannel* client, bool* data_open) {
    *data_open = true;
    owed       = OWED_DATA;
    for (;;) {
        uint32_t len;

        if (!engine_recv(&len, sizeof(len))) {
            return engine_death();
        }
        if (len > CHDB_CHUNK_MAX) {
            return engine_garbled();
        }

        char* chunk = scratch(len);

        if (len && !engine_recv(chunk, len)) {
            return engine_death();
        }
        if (len == 0) {
            owed = OWED_STATUS;
        }
        if (client) {
            chdb_channel_send_exact(client, &len, sizeof(len));
            chdb_channel_send_exact(client, chunk, len);
        }
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
    owed = dead ? OWED_NOTHING : OWED_CLIENT_CHUNKS;
    for (;;) {
        uint32_t len;

        chdb_channel_recv_exact(client, &len, sizeof(len));
        if (len > CHDB_CHUNK_MAX) {
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
    char* err;

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
    case CHDB_CMD_SELECT:
        err = relay_out(client, data_open);
        err = err ? err : relay_status(client);
        break;
    case CHDB_CMD_INSERT:
        err = relay_in(client, NULL);
        break;
    default:
        err = relay_status(client);
    }
    owed = OWED_NOTHING;

    return err;
}

bool
relay_succeeded(void) {
    return last_status == CHDB_STATUS_OK;
}

char*
relay_abandon(void) {
    bool open;
    char* err = NULL;

    switch (owed) {
    case OWED_NOTHING:
        break;
    case OWED_DATA:
        err = relay_out(NULL, &open);
        err = err ? err : relay_status(NULL);
        break;
    case OWED_STATUS:
        err = relay_status(NULL);
        break;
    case OWED_CLIENT_CHUNKS:
        /* Nobody will send them, and a made-up end would commit half an insert. */
        err = engine_death();
        break;
    }
    owed = OWED_NOTHING;

    return err;
}
