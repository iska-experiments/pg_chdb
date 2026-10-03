/*
 * The chdb_search background worker: one per database, owning that database's
 * chDB store and serving backends over a unix socket. See protocol.h for the
 * framing and dev/design/chdb_search.md for why there is a worker at all.
 *
 * The loop is single threaded and serves one request at a time. Connections
 * that are idle cost nothing, so a backend may keep its connection open
 * between requests without holding up the others.
 */

#include "postgres.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/file_perm.h"
#include "libpq/pqsignal.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/dsm_registry.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/spin.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "chdb.h"

#include "protocol.h"
#include "worker.h"

#define CHDB_SEARCH_MAX_WORKERS 64
#define CHDB_SEARCH_MAX_CLIENTS 128
#define CHDB_SEARCH_REGISTRY_NAME "chdb_search"

/* Native is the only format either direction crosses in. */
static const char native_format[] = "Native";

/* ---- registry of workers, shared by every backend --------------------- */

typedef enum workerState {
    WORKER_FREE = 0,
    WORKER_STARTING, /* a backend registered it; it has not claimed the slot yet */
    WORKER_RUNNING,
} workerState;

typedef struct workerSlot {
    Oid dboid;
    pid_t pid; /* the worker once RUNNING */
    workerState state;
    TimestampTz since; /* when STARTING began, so a lost start can be retried */
} workerSlot;

typedef struct workerRegistry {
    slock_t lock;
    workerSlot slots[CHDB_SEARCH_MAX_WORKERS];
} workerRegistry;

static void
registry_init(void* ptr) {
    workerRegistry* reg = ptr;

    SpinLockInit(&reg->lock);
    memset(reg->slots, 0, sizeof(reg->slots));
}

static workerRegistry*
registry(void) {
    bool found;

    return GetNamedDSMSegment(
        CHDB_SEARCH_REGISTRY_NAME, sizeof(workerRegistry), registry_init, &found
    );
}

/* The slot for `dboid`, else a free one, else NULL. Call with the lock held. */
static workerSlot*
find_slot(workerRegistry* reg, Oid dboid) {
    workerSlot* free = NULL;

    for (int i = 0; i < CHDB_SEARCH_MAX_WORKERS; i++) {
        workerSlot* slot = &reg->slots[i];

        if (slot->state != WORKER_FREE && slot->dboid == dboid) {
            return slot;
        }
        if (slot->state == WORKER_FREE && !free) {
            free = slot;
        }
    }

    return free;
}

/* A crashed worker leaves its slot RUNNING, so trust the process table. */
static bool
pid_alive(pid_t pid) {
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

static void
free_slot(Oid dboid, pid_t only_pid) {
    workerRegistry* reg = registry();

    SpinLockAcquire(&reg->lock);
    workerSlot* slot = find_slot(reg, dboid);
    if (slot && slot->state != WORKER_FREE &&
        (only_pid == 0 || slot->pid == only_pid)) {
        slot->state = WORKER_FREE;
        slot->pid   = 0;
    }
    SpinLockRelease(&reg->lock);
}

void
chdb_search_worker_ensure(Oid dboid) {
    workerRegistry* reg = registry();
    TimestampTz now     = GetCurrentTimestamp();
    bool mine           = false;
    bool full           = false;

    SpinLockAcquire(&reg->lock);
    workerSlot* slot = find_slot(reg, dboid);
    if (!slot) {
        full = true;
    } else if (slot->state == WORKER_RUNNING && pid_alive(slot->pid)) {
        /* Up. The caller's connect failing means it is about to listen. */
    } else if (
        slot->state == WORKER_STARTING &&
        !TimestampDifferenceExceeds(slot->since, now, chdb_search_worker_timeout * 1000)
    ) {
        /* Another backend is starting it. */
    } else {
        slot->dboid = dboid;
        slot->state = WORKER_STARTING;
        slot->pid   = 0;
        slot->since = now;
        mine        = true;
    }
    SpinLockRelease(&reg->lock);

    if (full) {
        ereport(
            ERROR,
            errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
            errmsg("chdb_search: too many databases have a worker"),
            errdetail(
                "At most %d databases can use chdb_search at once.",
                CHDB_SEARCH_MAX_WORKERS
            )
        );
    }
    if (!mine) {
        return;
    }

    BackgroundWorker bgw = {
        .bgw_flags      = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION,
        .bgw_start_time = BgWorkerStart_RecoveryFinished,
        .bgw_restart_time = 5,
        .bgw_main_arg     = ObjectIdGetDatum(dboid),
        .bgw_notify_pid   = MyProcPid,
    };
    BackgroundWorkerHandle* handle;
    pid_t pid;

    snprintf(bgw.bgw_library_name, BGW_MAXLEN, "chdb_search");
    snprintf(bgw.bgw_function_name, BGW_MAXLEN, "chdb_search_worker_main");
    snprintf(bgw.bgw_name, BGW_MAXLEN, "chdb_search worker for database %u", dboid);
    snprintf(bgw.bgw_type, BGW_MAXLEN, "chdb_search worker");

    if (!RegisterDynamicBackgroundWorker(&bgw, &handle)) {
        free_slot(dboid, 0);
        ereport(
            ERROR,
            errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
            errmsg("chdb_search: could not register the worker"),
            errhint("Increase max_worker_processes.")
        );
    }

    BgwHandleStatus status = WaitForBackgroundWorkerStartup(handle, &pid);
    if (status != BGWH_STARTED) {
        free_slot(dboid, 0);
        ereport(
            ERROR,
            errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
            errmsg("chdb_search: the worker did not start"),
            errhint("See the server log for why.")
        );
    }
}

/* ---- libchdb, loaded by dlopen since the backends must not link it ----- */

#define CHDB_SEARCH_SYMBOLS(X)                                                         \
    X(chdb_connect)                                                                    \
    X(chdb_close_conn)                                                                 \
    X(chdb_query_n)                                                                    \
    X(chdb_stream_query_n)                                                             \
    X(chdb_stream_fetch_result)                                                        \
    X(chdb_stream_cancel_query)                                                        \
    X(chdb_stream_insert_n)                                                            \
    X(chdb_stream_append)                                                              \
    X(chdb_stream_done)                                                                \
    X(chdb_stream_cancel_insert)                                                       \
    X(chdb_stream_insert_error)                                                        \
    X(chdb_result_buffer)                                                              \
    X(chdb_result_length)                                                              \
    X(chdb_result_error)                                                               \
    X(chdb_destroy_query_result)                                                       \
    X(chdb_destroy_insert_stream)                                                      \
    X(chdb_set_signal_handlers_enabled)

/* Pointer per entry point, typed from chdb.h so a signature drift fails to build. */
static struct {
#define X(name) __typeof__(&name) name;
    CHDB_SEARCH_SYMBOLS(X)
#undef X
} api;

static void
load_libchdb(void) {
    void* lib = dlopen(chdb_search_libchdb_path, RTLD_NOW | RTLD_LOCAL);

    if (!lib) {
        ereport(
            FATAL,
            errcode(ERRCODE_UNDEFINED_FILE),
            errmsg("chdb_search: could not load \"%s\"", chdb_search_libchdb_path),
            errdetail("%s", dlerror()),
            errhint("Set chdb_search.libchdb_path or LD_LIBRARY_PATH for the server.")
        );
    }

#define X(name)                                                                        \
    if (!(*(void**)& api.name = dlsym(lib, #name))) {                                  \
        ereport(                                                                       \
            FATAL,                                                                     \
            errcode(ERRCODE_UNDEFINED_FUNCTION),                                       \
            errmsg("chdb_search: \"%s\" lacks %s", chdb_search_libchdb_path, #name),   \
            errdetail("%s", dlerror())                                                 \
        );                                                                             \
    }
    CHDB_SEARCH_SYMBOLS(X)
#undef X
}

/* ---- worker state ------------------------------------------------------ */

static chdb_connection* chdb_conn;
static Oid worker_dboid;
static bool slot_claimed;
static int listen_fd = -1;
static char socket_path[MAXPGPATH];

/* The settings the chDB session carries now, so a request changes them only on need. */
static int applied_memory  = -1;
static int applied_threads = -1;
static int applied_parsers = -1;

static void
worker_exit(int code pg_attribute_unused(), Datum arg pg_attribute_unused()) {
    if (listen_fd >= 0) {
        unlink(socket_path);
    }
    if (chdb_conn) {
        api.chdb_close_conn(chdb_conn);
        chdb_conn = NULL;
    }
    if (slot_claimed) {
        free_slot(worker_dboid, MyProcPid);
    }
}

/*
 * The error as a client should see it, minus the Request ID line and chDB
 * version that differ between runs, as helper.c does.
 */
static char*
clean_error(const char* raw) {
    char* msg  = pstrdup(raw);
    size_t len = strlen(msg);

    while (len && (msg[len - 1] == '\n' || msg[len - 1] == '\r')) {
        msg[--len] = '\0';
    }

    const char* id  = strstr(msg, "Request ID:");
    const char* eol = id ? strchr(id, '\n') : NULL;
    if (eol) {
        memmove((char*)id, eol + 1, strlen(eol + 1) + 1);
    }

    const char* version = strstr(msg, " (version ");
    const char* close   = version ? strchr(version, ')') : NULL;
    if (close) {
        memmove((char*)version, close + 1, strlen(close + 1) + 1);
    }

    return msg;
}

/* Runs a statement, buffering its result. NULL on success, else the error. */
static char*
run_statement(const char* sql, size_t len) {
    chdb_result* res = api.chdb_query_n(
        *chdb_conn, sql, len, native_format, sizeof(native_format) - 1
    );
    const char* err = api.chdb_result_error(res);
    char* out       = err ? clean_error(err) : NULL;

    api.chdb_destroy_query_result(res);

    return out;
}

/* Settings are per session and arguments to chdb_connect do not take them. */
static char*
apply_settings(int memory, int threads, int parsers) {
    if (memory == applied_memory && threads == applied_threads &&
        parsers == applied_parsers) {
        return NULL;
    }

    char* sql = psprintf(
        "SET allow_experimental_nullable_tuple_type,"
        "output_format_json_quote_denormals,"
        "output_format_native_write_json_as_string,"
        "output_format_native_encode_types_in_binary_format=0,"
        "date_time_output_format='iso',"
        "max_threads=%d,max_parsing_threads=%d,max_memory_usage=%" PRIu64,
        threads,
        parsers,
        (uint64_t)memory * 1024 * 1024
    );
    char* err = run_statement(sql, strlen(sql));

    if (!err) {
        applied_memory  = memory;
        applied_threads = threads;
        applied_parsers = parsers;
    }
    pfree(sql);

    return err;
}

static void
open_store(void) {
    char* path = psprintf("--path=%s/pg_chdb/%u", DataDir, worker_dboid);

    /*
     * chDB's handlers would run on whichever thread takes a fatal signal and
     * replace Postgres's own.
     */
    api.chdb_set_signal_handlers_enabled(0);
    chdb_conn = api.chdb_connect(2, (char*[]){ "chdb", path, NULL });
    if (!chdb_conn || !*chdb_conn) {
        ereport(
            FATAL,
            errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
            errmsg("chdb_search: could not open the chDB store"),
            errdetail("path: %s", path + strlen("--path="))
        );
    }
    pfree(path);

    char* err = apply_settings(chdb_max_memory, chdb_max_threads, chdb_max_parsers);
    if (err) {
        ereport(
            FATAL,
            errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
            errmsg("chdb_search: could not configure chDB"),
            errdetail("%s", err)
        );
    }
}

/* ---- socket I/O --------------------------------------------------------- */

static void
wait_socket(int fd, uint32 event) {
    WaitLatchOrSocket(
        MyLatch, event | WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, fd, -1, PG_WAIT_EXTENSION
    );
    ResetLatch(MyLatch);
}

/*
 * Reads exactly `len` bytes. Returns 1 on success, 0 on a clean end of
 * stream before the first byte, and -1 for anything that leaves the framing
 * untrustworthy, including a shutdown request.
 */
static int
recv_full(int fd, void* buf, size_t len) {
    size_t got = 0;

    while (got < len) {
        CHECK_FOR_INTERRUPTS();
        if (ShutdownRequestPending) {
            return -1;
        }

        ssize_t n = recv(fd, (char*)buf + got, len - got, 0);
        if (n > 0) {
            got += (size_t)n;
        } else if (n == 0) {
            return got == 0 ? 0 : -1;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            wait_socket(fd, WL_SOCKET_READABLE);
        } else if (errno != EINTR) {
            return -1;
        }
    }

    return 1;
}

/* MSG_NOSIGNAL: a client that went away is an error here, not a SIGPIPE. */
static bool
send_full(int fd, const void* buf, size_t len) {
    size_t put = 0;

    while (put < len) {
        CHECK_FOR_INTERRUPTS();
        if (ShutdownRequestPending) {
            return false;
        }

        ssize_t n = send(fd, (const char*)buf + put, len - put, MSG_NOSIGNAL);
        if (n > 0) {
            put += (size_t)n;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            wait_socket(fd, WL_SOCKET_WRITEABLE);
        } else if (errno != EINTR) {
            return false;
        }
    }

    return true;
}

/* Sends one data chunk, splitting it so no chunk exceeds what clients accept. */
static bool
send_chunks(int fd, const char* buf, size_t len) {
    while (len) {
        uint32_t n = (uint32_t)Min(len, CHDB_SEARCH_CHUNK_MAX);

        if (!send_full(fd, &n, sizeof(n)) || !send_full(fd, buf, n)) {
            return false;
        }
        buf += n;
        len -= n;
    }

    return true;
}

static bool
send_end(int fd) {
    uint32_t zero = 0;

    return send_full(fd, &zero, sizeof(zero));
}

static bool
send_status(int fd, const char* err) {
    uint8_t status = err ? 1 : 0;
    uint32_t len   = err ? (uint32_t)strlen(err) : 0;

    return send_full(fd, &status, sizeof(status)) && send_full(fd, &len, sizeof(len)) &&
           (len == 0 || send_full(fd, err, len));
}

/* ---- requests ----------------------------------------------------------- */

typedef struct request {
    chdbCmdType cmd;
    Oid index;
    uint16_t max_memory;
    uint16_t max_threads;
    uint16_t max_parsers;
    char* query;
    size_t query_len;
} request;

/* Reads a length-prefixed string into request memory, NUL terminated. */
static int
recv_string(int fd, char** out, size_t* out_len) {
    uint32_t len;

    if (recv_full(fd, &len, sizeof(len)) != 1 || len > CHDB_SETUP_MAX) {
        return -1;
    }
    *out = palloc(len + 1);
    if (len && recv_full(fd, *out, len) != 1) {
        return -1;
    }
    (*out)[len] = '\0';
    if (out_len) {
        *out_len = len;
    }

    return 1;
}

/* 1 for a request, 0 when the client hung up between requests, -1 on a bad one. */
static int
recv_request(int fd, request* req, bool* has_params) {
    uint8_t head[1 + 4 + 2 + 2 + 2];
    int rc = recv_full(fd, head, sizeof(head));

    if (rc != 1) {
        return rc;
    }
    req->cmd = head[0];
    memcpy(&req->index, head + 1, 4);
    memcpy(&req->max_memory, head + 5, 2);
    memcpy(&req->max_threads, head + 7, 2);
    memcpy(&req->max_parsers, head + 9, 2);

    if (recv_string(fd, &req->query, &req->query_len) != 1) {
        return -1;
    }

    uint16_t nparams;
    if (recv_full(fd, &nparams, sizeof(nparams)) != 1) {
        return -1;
    }
    for (uint16_t i = 0; i < nparams; i++) {
        char* skip;

        if (recv_string(fd, &skip, NULL) != 1 || recv_string(fd, &skip, NULL) != 1) {
            return -1;
        }
    }
    *has_params = nparams > 0;

    return 1;
}

/* Settings and database every statement against an index needs first. */
static char*
prepare(const request* req) {
    char* err = apply_settings(req->max_memory, req->max_threads, req->max_parsers);

    if (err) {
        return err;
    }

    char* sql = psprintf("CREATE DATABASE IF NOT EXISTS idx_%u", req->index);
    err       = run_statement(sql, strlen(sql));
    pfree(sql);

    return err;
}

/* True while the connection is still in step. */
static bool
serve_exec(int fd, const request* req) {
    char* err = prepare(req);

    if (!err) {
        err = run_statement(req->query, req->query_len);
    }

    return send_status(fd, err);
}

static bool
serve_drop(int fd, const request* req) {
    char* err = apply_settings(req->max_memory, req->max_threads, req->max_parsers);

    if (!err) {
        char* sql = psprintf("DROP DATABASE IF EXISTS idx_%u SYNC", req->index);
        err       = run_statement(sql, strlen(sql));
        pfree(sql);
    }

    return send_status(fd, err);
}

static bool
serve_select(int fd, const request* req) {
    char* err = prepare(req);
    bool lost = false;

    if (!err) {
        chdb_result* stream = api.chdb_stream_query_n(
            *chdb_conn,
            req->query,
            req->query_len,
            native_format,
            sizeof(native_format) - 1
        );
        const char* stream_err = api.chdb_result_error(stream);

        if (stream_err) {
            err = clean_error(stream_err);
        }

        while (!err && !lost) {
            CHECK_FOR_INTERRUPTS();
            if (ShutdownRequestPending) {
                lost = true;
                break;
            }

            chdb_result* chunk    = api.chdb_stream_fetch_result(*chdb_conn, stream);
            const char* chunk_err = api.chdb_result_error(chunk);
            size_t len            = chunk_err ? 0 : api.chdb_result_length(chunk);

            if (chunk_err) {
                err = clean_error(chunk_err);
            } else if (len == 0) {
                api.chdb_destroy_query_result(chunk);
                break; /* end of stream */
            } else if (!send_chunks(fd, api.chdb_result_buffer(chunk), len)) {
                lost = true;
            }
            api.chdb_destroy_query_result(chunk);
        }

        if (err || lost) {
            api.chdb_stream_cancel_query(*chdb_conn, stream);
        }
        api.chdb_destroy_query_result(stream);
    }

    /* A failure after data was sent still ends the data and reports itself. */
    return !lost && send_end(fd) && send_status(fd, err);
}

static bool
serve_insert(int fd, const request* req) {
    char* err                 = prepare(req);
    chdb_insert_stream stream = NULL;
    bool live                 = false;
    char* buf                 = NULL;
    size_t cap                = 0;
    bool in_step              = true;

    if (!err) {
        stream = api.chdb_stream_insert_n(
            *chdb_conn,
            req->query,
            req->query_len,
            native_format,
            sizeof(native_format) - 1
        );
        const char* open_err = api.chdb_stream_insert_error(stream);

        if (open_err) {
            err = clean_error(open_err);
        } else {
            live = true;
        }
    }

    /* Read to the end-of-data chunk even after a failure, to stay in step. */
    for (;;) {
        uint32_t len;

        if (recv_full(fd, &len, sizeof(len)) != 1 || len > CHDB_SEARCH_CHUNK_MAX) {
            in_step = false;
            break;
        }
        if (len == 0) {
            break;
        }
        if (len > cap) {
            if (buf) {
                pfree(buf);
            }
            cap = len;
            buf = palloc(cap);
        }
        if (recv_full(fd, buf, len) != 1) {
            in_step = false;
            break;
        }
        if (live && api.chdb_stream_append(stream, buf, len) != CHDBSuccess) {
            const char* append_err = api.chdb_stream_insert_error(stream);

            err = clean_error(append_err ? append_err : "could not append to chDB");
            api.chdb_stream_cancel_insert(stream);
            live = false;
        }
    }

    if (stream) {
        if (live && !in_step) {
            api.chdb_stream_cancel_insert(stream);
        } else if (live) {
            chdb_result* done    = api.chdb_stream_done(stream);
            const char* done_err = api.chdb_result_error(done);

            if (done_err) {
                err = clean_error(done_err);
            }
            api.chdb_destroy_query_result(done);
        }
        api.chdb_destroy_insert_stream(stream);
    }

    return in_step && send_status(fd, err);
}

/* Serves one request on `fd`. False means close the connection. */
static bool
serve_request(int fd, MemoryContext cxt) {
    MemoryContext old = MemoryContextSwitchTo(cxt);
    request req       = { 0 };
    bool has_params   = false;
    bool keep         = false;

    if (recv_request(fd, &req, &has_params) == 1) {
        if (has_params) {
            keep = send_status(fd, "query parameters are not supported");
        } else {
            switch (req.cmd) {
            case CHDB_CMD_EXEC:
                keep = serve_exec(fd, &req);
                break;
            case CHDB_CMD_SELECT:
                keep = serve_select(fd, &req);
                break;
            case CHDB_CMD_INSERT:
                keep = serve_insert(fd, &req);
                break;
            case CHDB_CMD_DROP:
                keep = serve_drop(fd, &req);
                break;
            default:
                /* Unknown commands carry unknown data, so the framing is gone. */
                send_status(fd, "unknown command");
                break;
            }
        }
    }

    MemoryContextSwitchTo(old);
    MemoryContextReset(cxt);

    return keep;
}

/* ---- the loop ----------------------------------------------------------- */

static void
open_listener(void) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };

    snprintf(socket_path, sizeof(socket_path), CHDB_SEARCH_SOCKET_FMT, worker_dboid);
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        ereport(
            FATAL, errmsg("chdb_search: socket path \"%s\" is too long", socket_path)
        );
    }
    strcpy(addr.sun_path, socket_path);

    if (MakePGDirectory("pg_chdb") < 0 && errno != EEXIST) {
        ereport(
            FATAL,
            errcode_for_file_access(),
            errmsg("chdb_search: could not create directory \"pg_chdb\": %m")
        );
    }

    /* A crashed predecessor leaves its socket file behind. */
    unlink(socket_path);
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listen_fd < 0 || bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(listen_fd, 64) < 0) {
        ereport(
            FATAL,
            errcode_for_socket_access(),
            errmsg("chdb_search: could not listen on \"%s\": %m", socket_path)
        );
    }
}

/* WL_SOCKET_ACCEPT is WL_SOCKET_READABLE, so user_data tells the listener apart. */
#define LISTENER ((void*)(intptr_t)-1)

/* Rebuilt on every change, as WaitEventSets cannot drop a socket. */
static WaitEventSet*
build_wait_set(const int* clients, int nclients) {
    WaitEventSet* set = CreateWaitEventSet(NULL, nclients + 3);

    AddWaitEventToSet(set, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
    AddWaitEventToSet(set, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET, NULL, NULL);
    AddWaitEventToSet(set, WL_SOCKET_ACCEPT, listen_fd, NULL, LISTENER);
    for (int i = 0; i < nclients; i++) {
        AddWaitEventToSet(
            set, WL_SOCKET_READABLE, clients[i], NULL, (void*)(intptr_t)i
        );
    }

    return set;
}

void
chdb_search_worker_main(Datum arg) {
    int clients[CHDB_SEARCH_MAX_CLIENTS];
    int nclients = 0;

    worker_dboid = DatumGetObjectId(arg);

    pqsignal(SIGHUP, SignalHandlerForConfigReload);
    pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnectionByOid(worker_dboid, InvalidOid, 0);

    /* Before shared memory goes, since it frees the registry slot. */
    before_shmem_exit(worker_exit, (Datum)0);

    workerRegistry* reg = registry();
    bool taken          = false;

    SpinLockAcquire(&reg->lock);
    workerSlot* slot = find_slot(reg, worker_dboid);
    if (slot && !(slot->state == WORKER_RUNNING && slot->pid != MyProcPid &&
                  pid_alive(slot->pid))) {
        slot->dboid = worker_dboid;
        slot->state = WORKER_RUNNING;
        slot->pid   = MyProcPid;
        taken       = true;
    }
    SpinLockRelease(&reg->lock);
    if (!taken) {
        /* Postmaster restarted a worker whose replacement is already up. */
        proc_exit(0);
    }
    slot_claimed = true;

    load_libchdb();
    open_listener();
    open_store();
    ereport(LOG, errmsg("chdb_search: worker for database %u listening", worker_dboid));

    MemoryContext request_cxt = AllocSetContextCreate(
        TopMemoryContext, "chdb_search request", ALLOCSET_DEFAULT_SIZES
    );
    WaitEventSet* set = build_wait_set(clients, nclients);

    while (!ShutdownRequestPending) {
        WaitEvent event;
        bool rebuild = false;

        WaitEventSetWait(set, -1, &event, 1, PG_WAIT_EXTENSION);
        CHECK_FOR_INTERRUPTS();

        if (event.events & WL_LATCH_SET) {
            ResetLatch(MyLatch);
            if (ConfigReloadPending) {
                ConfigReloadPending = false;
                ProcessConfigFile(PGC_SIGHUP);
            }
        } else if ((event.events & WL_SOCKET_READABLE) && event.user_data == LISTENER) {
            int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

            if (fd >= 0 && nclients < CHDB_SEARCH_MAX_CLIENTS) {
                clients[nclients++] = fd;
                rebuild             = true;
            } else if (fd >= 0) {
                close(fd);
            }
        } else if (event.events & WL_SOCKET_READABLE) {
            int i = (int)(intptr_t)event.user_data;

            if (!serve_request(clients[i], request_cxt)) {
                close(clients[i]);
                clients[i] = clients[--nclients];
                rebuild    = true;
            }
        }

        if (rebuild) {
            FreeWaitEventSet(set);
            set = build_wait_set(clients, nclients);
        }
    }

    ereport(
        LOG, errmsg("chdb_search: worker for database %u shutting down", worker_dboid)
    );
    for (int i = 0; i < nclients; i++) {
        close(clients[i]);
    }
    proc_exit(0);
}
