/*
 * The worker's accept and dispatch loop: one thread, one request at a time,
 * with idle connections costing nothing.
 */

#include "postgres.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "miscadmin.h"
#include "postmaster/interrupt.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#include "protocol.h"
#include "request.h"
#include "serve.h"

#define CHDB_SEARCH_MAX_CLIENTS 128

static int listen_fd = -1;
static char socket_path[MAXPGPATH];

void
chdb_search_listen(Oid dboid) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };

    snprintf(socket_path, sizeof(socket_path), CHDB_SEARCH_SOCKET_FMT, dboid);
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        ereport(
            FATAL, errmsg("chdb_search: socket path \"%s\" is too long", socket_path)
        );
    }
    strcpy(addr.sun_path, socket_path);

    if (MakePGDirectory(CHDB_SEARCH_DIR) < 0 && errno != EEXIST) {
        ereport(
            FATAL,
            errcode_for_file_access(),
            errmsg(
                "chdb_search: could not create directory \"%s\": %m", CHDB_SEARCH_DIR
            )
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
chdb_search_unlisten(void) {
    if (listen_fd >= 0) {
        unlink(socket_path);
    }
}

void
chdb_search_serve(void) {
    int clients[CHDB_SEARCH_MAX_CLIENTS] = { 0 };
    int nclients                         = 0;

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

            if (!chdb_search_serve_request(clients[i], request_cxt)) {
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
}
