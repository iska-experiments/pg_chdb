/*
 * The engine's end of the page request protocol (../pagestore/protocol.h):
 * requests written whole under one mutex from whichever thread makes them,
 * replies read by one thread of this file's and handed to the caller waiting
 * on the matching id. The supervisor answers in order, but a reply may still
 * be for any of the calls in flight. See pagecall.h.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../pagestore/protocol.h"
#include "io.h"
#include "pagecall.h"

/* A call in flight, on its caller's stack. */
typedef struct Pending {
    uint32_t id;
    pageReply* reply;
    bool done;
    pthread_cond_t cond;
    struct Pending* next;
} Pending;

static int page_fd                = -1;
static pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t list_lock  = PTHREAD_MUTEX_INITIALIZER;
static Pending* pendings;
static uint32_t next_id = 1;
static bool gone; /* the supervisor's end broke, or its framing did */

static __thread char last_error[1024];

const char*
pagecall_last_error(void) {
    return last_error;
}

void
pagecall_set_error(const char* text, size_t len) {
    snprintf(
        last_error, sizeof(last_error), "%.*s", (int)(len < 1000 ? len : 1000), text
    );
}

/* Wakes every caller; with `gone` set, each finds its call undone. */
static void
fail_all(void) {
    pthread_mutex_lock(&list_lock);
    gone = true;
    for (Pending* p = pendings; p; p = p->next) {
        pthread_cond_signal(&p->cond);
    }
    pthread_mutex_unlock(&list_lock);
}

/*
 * Where a reply's body goes: the caller's buffer for a successful fixed
 * reply, else fresh memory, since an error text has no bound the caller
 * knew. NULL when it cannot be taken.
 */
static char*
reply_room(pageReply* r, const chdbPageReply* head) {
    if (r->fixed && head->status == CHDB_PAGE_OK) {
        return head->len <= r->cap ? r->body : NULL;
    }
    r->fixed = false;
    r->body  = malloc(head->len + 1);
    if (r->body) {
        r->body[head->len] = '\0';
    }
    return r->body;
}

static void*
reader(void* arg) {
    (void)arg;
    for (;;) {
        chdbPageReply head;
        Pending* p;

        if (io_recv(page_fd, &head, sizeof(head)) != 1 ||
            head.len > CHDB_PAGE_BODY_MAX) {
            break;
        }
        pthread_mutex_lock(&list_lock);
        for (p = pendings; p && p->id != head.id; p = p->next) {}
        pthread_mutex_unlock(&list_lock);
        if (!p) {
            break; /* an id nobody waits for: the framing is gone */
        }

        /* Ours alone until done is set: the caller sleeps, nobody else knows it. */
        char* into = reply_room(p->reply, &head);

        if (!into || (head.len && io_recv(page_fd, into, head.len) != 1)) {
            break;
        }
        p->reply->len    = head.len;
        p->reply->status = head.status;
        pthread_mutex_lock(&list_lock);
        p->done = true;
        pthread_cond_signal(&p->cond);
        pthread_mutex_unlock(&list_lock);
    }
    fail_all();

    return NULL;
}

bool
pagecall_start(int fd) {
    pthread_t thread;
    int rc;

    page_fd = fd;
    rc      = pthread_create(&thread, NULL, reader, NULL);
    if (rc != 0) {
        fprintf(
            stderr,
            "chdb_search_engine: could not start the page reader: %s\n",
            strerror(rc)
        );
        return false;
    }
    pthread_detach(thread);

    return true;
}

/*
 * Links the call and sends it. False when the supervisor is gone, whether
 * known already, the call then never linked, or found out by this send.
 */
static bool
send_call(
    Pending* p,
    chdbPageRequest* req,
    const void* head,
    size_t headlen,
    const void* data,
    size_t datalen
) {
    pthread_mutex_lock(&list_lock);
    if (gone) {
        pthread_mutex_unlock(&list_lock);
        return false;
    }
    p->id = req->id = next_id++;
    p->next         = pendings;
    pendings        = p;
    pthread_mutex_unlock(&list_lock);

    pthread_mutex_lock(&write_lock);
    bool ok = io_send(page_fd, req, sizeof(*req)) && io_send(page_fd, head, headlen) &&
              (datalen == 0 || io_send(page_fd, data, datalen));
    pthread_mutex_unlock(&write_lock);

    return ok;
}

/* Waits for the reply and unlinks the call, if linked. False when it never came. */
static bool
await_reply(Pending* p, bool sent) {
    pthread_mutex_lock(&list_lock);
    if (!sent) {
        gone = true;
    }
    while (!p->done && !gone) {
        pthread_cond_wait(&p->cond, &list_lock);
    }
    for (Pending** at = &pendings; *at; at = &(*at)->next) {
        if (*at == p) {
            *at = p->next;
            break;
        }
    }
    pthread_mutex_unlock(&list_lock);

    return p->done;
}

uint32_t
pagecall(
    uint32_t op,
    const void* head,
    size_t headlen,
    const void* data,
    size_t datalen,
    pageReply* r
) {
    Pending p           = { .reply = r };
    chdbPageRequest req = { .op = op, .len = (uint32_t)(headlen + datalen) };

    r->status = CHDB_PAGE_ERROR;
    r->len    = 0;
    if (!r->fixed) {
        r->body = NULL;
    }
    pthread_cond_init(&p.cond, NULL);

    bool ok = await_reply(&p, send_call(&p, &req, head, headlen, data, datalen));

    pthread_cond_destroy(&p.cond);
    if (!ok) {
        const char msg[] = "the chdb_search worker is gone";

        pagecall_set_error(msg, sizeof(msg) - 1);
        return CHDB_PAGE_ERROR;
    }
    if (r->status != CHDB_PAGE_OK) {
        pagecall_set_error(r->body ? r->body : "", r->len);
        free(r->body);
        r->body = NULL;
    }

    return r->status;
}

/* ---- the body of a request ---- */

void
pagebody_put(pageBody* b, const void* p, size_t n) {
    if (b->len + n > sizeof(b->data)) {
        b->overflow = true;
    } else {
        memcpy(b->data + b->len, p, n);
        b->len += n;
    }
}

void
pagebody_str(pageBody* b, const char* s) {
    uint32_t n = (uint32_t)strlen(s);

    pagebody_put(b, &n, sizeof(n));
    pagebody_put(b, s, n);
}

void
pagebody_u64(pageBody* b, uint64_t v) {
    pagebody_put(b, &v, sizeof(v));
}

int
pagecall_fail(const char* msg) {
    pagecall_set_error(msg, strlen(msg));
    return 1;
}

int
pagecall_body(
    uint32_t op,
    const pageBody* b,
    const void* data,
    size_t datalen,
    pageReply* r
) {
    if (b->overflow) {
        return pagecall_fail("page request too large");
    }
    return pagecall(op, b->data, b->len, data, datalen, r) == CHDB_PAGE_OK ? 0 : 1;
}
