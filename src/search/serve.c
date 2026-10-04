/*
 * The worker's accept and dispatch loop: one thread, one request at a time,
 * with idle connections costing nothing. On a server in recovery the wait
 * is bounded, so that the loop notices the promotion (standby.c).
 *
 * On Linux the worker listens on a name in the abstract namespace,
 * `pg_chdb/<hash>/<dboid>`, the hash that of the data directory's path,
 * device and inode, so that two clusters on one host, or two containers
 * sharing a network namespace whose data directories have the same path,
 * get names of their own. An abstract socket is no file: nothing in the
 * data directory for a backup to trip over (WAL-G's backup-push aborts on a
 * socket, as tar has no entry for one), and nothing to unlink, as the
 * kernel drops the name with the last descriptor, a crashed worker's too.
 * Having no file mode either, it lets in only peers of the server's own
 * user, which the data directory's mode let in before. Elsewhere the socket
 * is the file <dboid>.sock beside the engines' directories (protocol.h).
 */

#include "postgres.h"

#include <errno.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/file_perm.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "postmaster/interrupt.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#include "engine_proc.h"
#include "protocol.h"
#include "request.h"
#include "serve.h"
#include "standby.h"

#define CHDB_SEARCH_MAX_CLIENTS 128

static int listen_fd = -1;
static struct sockaddr_un listen_addr;

socklen_t
chdb_search_socket_addr(Oid dboid, struct sockaddr_un* addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
#ifdef __linux__
    struct stat st;

    if (stat(DataDir, &st) < 0) {
        ereport(
            ERROR,
            errcode_for_file_access(),
            errmsg("chdb_search: could not stat directory \"%s\": %m", DataDir)
        );
    }

    uint64 hash =
        hash_bytes_extended((const unsigned char*)DataDir, strlen(DataDir), 0);

    hash = hash_combine64(hash_combine64(hash, st.st_dev), st.st_ino);
    /* sun_path[0] stays NUL: the name is abstract and runs to the length. */
    return offsetof(struct sockaddr_un, sun_path) + 1 +
           snprintf(
               addr->sun_path + 1,
               sizeof(addr->sun_path) - 1,
               CHDB_SEARCH_DIR "/%016llx/%u",
               (unsigned long long)hash,
               dboid
           );
#else
    snprintf(addr->sun_path, sizeof(addr->sun_path), CHDB_SEARCH_SOCKET_FMT, dboid);
    return sizeof(*addr);
#endif
}

/* The socket's name for a message: an abstract one as `ss -x` shows it, `@...`. */
static const char*
socket_name(const struct sockaddr_un* addr) {
    return addr->sun_path[0] ? addr->sun_path : psprintf("@%s", addr->sun_path + 1);
}

void
chdb_search_listen(Oid dboid) {
    socklen_t len = chdb_search_socket_addr(dboid, &listen_addr);

    /* The worker's directory, in the data directory's mode. */
    char dir[MAXPGPATH];

    /* pg_mkdir_p writes into the path it is given. */
    strlcpy(dir, CHDB_SEARCH_CACHE_DIR, sizeof(dir));
    if (pg_mkdir_p(dir, pg_dir_create_mode) < 0 && errno != EEXIST) {
        ereport(
            FATAL,
            errcode_for_file_access(),
            errmsg("chdb_search: could not create directory \"%s\": %m", dir)
        );
    }

    /* A crashed predecessor leaves a socket file behind. */
    if (listen_addr.sun_path[0]) {
        unlink(listen_addr.sun_path);
    }
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listen_fd < 0 || bind(listen_fd, (struct sockaddr*)&listen_addr, len) < 0 ||
        listen(listen_fd, 64) < 0) {
        ereport(
            FATAL,
            errcode_for_socket_access(),
            errmsg(
                "chdb_search: could not listen on \"%s\": %m", socket_name(&listen_addr)
            )
        );
    }
}

/* Whether a peer may send requests: on Linux, one of the server's user. */
static bool
peer_allowed(int fd) {
#ifdef __linux__
    struct ucred cred;
    socklen_t len = sizeof(cred);

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 ||
        cred.uid != geteuid()) {
        ereport(
            LOG, errmsg("chdb_search: refused a connection from another user's process")
        );
        return false;
    }
#endif
    return true;
}

/*
 * WL_SOCKET_ACCEPT is WL_SOCKET_READABLE, so user_data tells the listener
 * apart, and the engine's page channel, which it asks on for its blobs while
 * no request runs, for the merges it does in the background.
 */
#define LISTENER ((void*)(intptr_t)-1)
#define PAGES ((void*)(intptr_t)-2)

/* Rebuilt on every change, as WaitEventSets cannot drop a socket. */
static WaitEventSet*
build_wait_set(const int* clients, int nclients, int page_fd) {
    WaitEventSet* set = CreateWaitEventSet(NULL, nclients + 4);

    AddWaitEventToSet(set, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
    AddWaitEventToSet(set, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET, NULL, NULL);
    AddWaitEventToSet(set, WL_SOCKET_ACCEPT, listen_fd, NULL, LISTENER);
    if (page_fd >= 0) {
        AddWaitEventToSet(set, WL_SOCKET_READABLE, page_fd, NULL, PAGES);
    }
    for (int i = 0; i < nclients; i++) {
        AddWaitEventToSet(
            set, WL_SOCKET_READABLE, clients[i], NULL, (void*)(intptr_t)i
        );
    }

    return set;
}

/*
 * Closing the socket is what frees an abstract name, which outlives an
 * unlink until the last descriptor goes; a client that connects in the
 * meantime is taken into the backlog of a worker that will never read it,
 * and gets a reset where it would have started the next worker.
 */
void
chdb_search_unlisten(void) {
    if (listen_fd < 0) {
        return;
    }
    if (listen_addr.sun_path[0]) {
        unlink(listen_addr.sun_path);
    }
    close(listen_fd);
    listen_fd = -1;
}

void
chdb_search_serve(void) {
    int clients[CHDB_SEARCH_MAX_CLIENTS] = { 0 };
    int nclients                         = 0;
    int page_fd                          = engine_page_fd();

    MemoryContext request_cxt = AllocSetContextCreate(
        TopMemoryContext, "chdb_search request", ALLOCSET_DEFAULT_SIZES
    );
    WaitEventSet* set = build_wait_set(clients, nclients, page_fd);

    while (!ShutdownRequestPending) {
        WaitEvent event;
        bool rebuild = false;
        int n        = WaitEventSetWait(
            set, chdb_search_standby_timeout(), &event, 1, PG_WAIT_EXTENSION
        );

        CHECK_FOR_INTERRUPTS();
        chdb_search_standby_poll();

        if (n == 0) {
            /* The timeout: nothing to serve. */
        } else if (event.events & WL_LATCH_SET) {
            ResetLatch(MyLatch);
            if (ConfigReloadPending) {
                ConfigReloadPending = false;
                ProcessConfigFile(PGC_SIGHUP);
            }
        } else if ((event.events & WL_SOCKET_READABLE) && event.user_data == LISTENER) {
            int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

            if (fd >= 0 && nclients < CHDB_SEARCH_MAX_CLIENTS && peer_allowed(fd)) {
                clients[nclients++] = fd;
                rebuild             = true;
            } else if (fd >= 0) {
                close(fd);
            }
        } else if ((event.events & WL_SOCKET_READABLE) && event.user_data == PAGES) {
            engine_serve_page();
        } else if (event.events & WL_SOCKET_READABLE) {
            int i = (int)(intptr_t)event.user_data;

            if (!chdb_search_serve_request(clients[i], request_cxt)) {
                close(clients[i]);
                clients[i] = clients[--nclients];
                rebuild    = true;
            }
        }

        /* A request may have started the engine, or found it gone. */
        if (engine_page_fd() != page_fd) {
            page_fd = engine_page_fd();
            rebuild = true;
        }
        if (rebuild) {
            FreeWaitEventSet(set);
            set = build_wait_set(clients, nclients, page_fd);
        }
    }
}
