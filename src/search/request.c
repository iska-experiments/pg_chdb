/*
 * One request of a client connection: reading it, starting the engine if it
 * is not running, forwarding, and turning an engine failure into the error
 * status the client expects. See ../search/protocol.h.
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
#include "worker.h"

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

/* Appends `len` bytes of the client's stream to `raw`. */
static void
slurp(chdbChannel* client, StringInfo raw, size_t len) {
    enlargeStringInfo(raw, (int)len);
    chdb_channel_recv_exact(client, raw->data + raw->len, len);
    raw->len += (int)len;
    raw->data[raw->len] = '\0';
}

/* A length-prefixed string; `*at` is where it starts in `raw`. */
static void
slurp_string(chdbChannel* client, StringInfo raw, int* at) {
    uint32_t len;

    chdb_channel_recv_exact(client, &len, sizeof(len));
    if (len > CHDB_SETUP_MAX) {
        ereport(ERROR, errmsg("chdb_search: client sent a string too large to take"));
    }
    appendBinaryStringInfo(raw, (char*)&len, sizeof(len));
    *at = raw->len;
    slurp(client, raw, len);
}

/* Reads a request whole, verbatim, for forwarding. Returns its command. */
static int
slurp_request(chdbChannel* client, StringInfo raw, int* query_at, bool* has_params) {
    int skip;
    uint16_t nparams;

    slurp(client, raw, 1 + 4 + 2 + 2 + 2);
    slurp_string(client, raw, query_at);
    slurp(client, raw, sizeof(nparams));
    memcpy(&nparams, raw->data + raw->len - sizeof(nparams), sizeof(nparams));
    for (uint16_t i = 0; i < nparams; i++) {
        slurp_string(client, raw, &skip);
        slurp_string(client, raw, &skip);
    }
    *has_params = nparams > 0;

    return (uint8_t)raw->data[0];
}

static void
send_status(chdbChannel* client, bool ok, const char* text) {
    uint8_t status = ok ? 0 : 1;
    uint32_t len   = (uint32_t)strlen(text);

    chdb_channel_send_exact(client, &status, sizeof(status));
    chdb_channel_send_exact(client, &len, sizeof(len));
    chdb_channel_send_exact(client, text, len);
}

/* The debug commands, which concern the engine rather than chDB. */
static void
control(chdbChannel* client, int cmd, const char* arg) {
    pid_t pid = engine_pid();

    if (cmd == CHDB_CMD_ENGINE_PID) {
        send_status(client, true, psprintf("%d", (int)pid));
    } else if (pid == 0) {
        send_status(client, false, "no chDB engine is running");
    } else if (kill(pid, atoi(arg)) != 0) {
        send_status(client, false, psprintf("could not signal the engine: %m"));
    } else {
        send_status(client, true, psprintf("%d", (int)pid));
    }
}

/* Answers one request. Raises for a client that fails. */
static bool
serve(chdbChannel* client, MemoryContext cxt) {
    StringInfoData raw;
    int query_at;
    bool has_params;
    bool data_open;
    int cmd;

    MemoryContextSwitchTo(cxt);
    initStringInfo(&raw);
    cmd = slurp_request(client, &raw, &query_at, &has_params);

    if (has_params) {
        send_status(client, false, "query parameters are not supported");
        return true;
    }
    if (cmd == CHDB_CMD_ENGINE_PID || cmd == CHDB_CMD_ENGINE_KILL) {
        control(client, cmd, raw.data + query_at);
        return true;
    }
    if (cmd != CHDB_CMD_EXEC && cmd != CHDB_CMD_SELECT && cmd != CHDB_CMD_INSERT &&
        cmd != CHDB_CMD_DROP) {
        /* Unknown commands carry unknown data, so the framing is gone. */
        send_status(client, false, "unknown command");
        return false;
    }

    engine_busy = true;
    char* err   = relay_request(client, &raw, cmd, &data_open);
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
