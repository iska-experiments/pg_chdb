/*
 * One request of a client connection: reading it, starting the engine if it
 * is not running, forwarding, and turning an engine failure into the error
 * status the client expects. See protocol.h.
 */

#include "postgres.h"

#include <signal.h>

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#include "../channel.h"
#include "engine_proc.h"
#include "protocol.h"
#include "relay.h"
#include "request.h"

/* Set when the failure is the client's, which has nobody left to be told. */
static bool client_gone;

/* Set while the engine is mid-request, so that a failure leaves it out of step. */
static bool engine_busy;

static void
client_lost(chdbChannel* ch pg_attribute_unused(), const char* what, int errnum) {
    client_gone = true;
    ereport(
        ERROR,
        errmsg("chdb_search: %s", what),
        errdetail("%s", errnum ? strerror(errnum) : "end of stream")
    );
}

/*
 * Reads a request whole into `raw`, byte count included, as the engine will
 * read it, and decodes what the worker itself needs of it.
 */
static void
slurp_request(chdbChannel* client, StringInfo raw, chdbSearchRequest* req) {
    uint32_t len;

    chdb_channel_recv_exact(client, &len, sizeof(len));
    if (len > CHDB_SETUP_MAX) {
        ereport(ERROR, errmsg("chdb_search: client sent a request too large to take"));
    }
    appendBinaryStringInfo(raw, (char*)&len, sizeof(len));
    enlargeStringInfo(raw, (int)len);
    chdb_channel_recv_exact(client, raw->data + raw->len, len);
    raw->len += (int)len;
    raw->data[raw->len] = '\0';

    if (!chdb_search_decode_request(raw->data + sizeof(len), len, req)) {
        ereport(ERROR, errmsg("chdb_search: client sent a malformed request"));
    }
}

static void
send_status(chdbChannel* client, bool ok, const char* text) {
    uint8_t status = ok ? CHDB_STATUS_OK : CHDB_STATUS_ERROR;
    uint32_t len   = (uint32_t)strlen(text);

    chdb_channel_send_exact(client, &status, sizeof(status));
    chdb_channel_send_exact(client, &len, sizeof(len));
    chdb_channel_send_exact(client, text, len);
}

/* The debug commands, which concern the engine rather than chDB. */
static void
control(chdbChannel* client, const chdbSearchRequest* req) {
    pid_t pid = engine_pid();

    if (req->ctx.cmd == CHDB_CMD_ENGINE_PID) {
        send_status(client, true, psprintf("%d", (int)pid));
    } else if (pid == 0) {
        send_status(client, false, "no chDB engine is running");
    } else if (kill(pid, atoi(pnstrdup(req->query.data, req->query.len))) != 0) {
        send_status(client, false, psprintf("could not signal the engine: %m"));
    } else {
        send_status(client, true, psprintf("%d", (int)pid));
    }
}

/* Answers one request. Raises for a client that fails. */
static bool
serve(chdbChannel* client, MemoryContext cxt) {
    StringInfoData raw;
    chdbSearchRequest req;
    bool data_open;

    MemoryContextSwitchTo(cxt);
    initStringInfo(&raw);
    slurp_request(client, &raw, &req);

    switch (req.ctx.cmd) {
    case CHDB_CMD_ENGINE_PID:
    case CHDB_CMD_ENGINE_KILL:
        control(client, &req);
        return true;
    case CHDB_CMD_EXEC:
    case CHDB_CMD_SELECT:
    case CHDB_CMD_INSERT:
    case CHDB_CMD_DROP:
        break;
    default:
        /* Unknown commands carry unknown data, so the framing is gone. */
        send_status(client, false, "unknown command");
        return false;
    }

    engine_busy = true;
    char* err   = relay_request(client, &raw, req.ctx.cmd, &data_open);
    engine_busy = false;

    if (err) {
        uint32_t zero = 0;

        if (data_open) {
            chdb_channel_send_exact(client, &zero, sizeof(zero));
        }
        send_status(client, false, err);
    }

    return true;
}

bool
chdb_search_serve_request(int fd, MemoryContext cxt) {
    MemoryContext old = CurrentMemoryContext;
    chdbChannel client;
    bool keep = false;

    chdb_channel_init(&client, fd, -1);
    client.recv_what = "error receiving from the client";
    client.send_what = "error sending to the client";
    client.fail      = client_lost;
    client_gone      = false;
    engine_busy      = false;

    PG_TRY();
    { keep = serve(&client, cxt); }
    PG_CATCH();
    {
        MemoryContextSwitchTo(old);
        if (!client_gone) {
            EmitErrorReport();
        }
        FlushErrorState();
        /* The engine may be halfway through a request that nobody finishes. */
        if (engine_busy && engine_pid() > 0) {
            pfree(engine_death());
        }
        keep = false;
    }
    PG_END_TRY();

    MemoryContextSwitchTo(old);
    MemoryContextReset(cxt);

    return keep;
}
