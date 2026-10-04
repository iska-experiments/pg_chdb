/*
 * The waiting, interrupt handling, error capture and cleanup shared by every
 * channel to a chDB process. See channel.h.
 */

#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "storage/latch.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#include "channel.h"

/* Milliseconds between wakeups while a channel is idle, so errors still drain. */
#define CHDB_CHANNEL_POLL_MS 1000

void
chdb_channel_close_fd(int* fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

void
chdb_channel_init(chdbChannel* ch, int data, int err) {
    memset(ch, 0, sizeof(*ch));
    ch->data      = data;
    ch->err       = err;
    ch->recv_what = "error receiving from chDB";
    ch->send_what = "error sending to chDB";
    ch->wait_what = "timed out waiting for chDB";
}

void
chdb_channel_close(chdbChannel* ch) {
    chdb_channel_close_fd(&ch->data);
    chdb_channel_close_fd(&ch->err);
}

static void
cleanup(void* arg) {
    chdbChannel* ch = arg;

    chdb_channel_close(ch);
    if (ch->stop) {
        ch->stop(ch);
    }
}

void
chdb_channel_own(chdbChannel* ch) {
    ch->cleanup.func = cleanup;
    ch->cleanup.arg  = ch;
    MemoryContextRegisterResetCallback(CurrentMemoryContext, &ch->cleanup);
}

static void
set_flag(int fd, int get, int set, int flag) {
    int flags = fcntl(fd, get);

    if (flags < 0 || fcntl(fd, set, flags | flag) < 0) {
        ereport(
            ERROR,
            errcode_for_socket_access(),
            errmsg("chdb: could not configure the chDB channel: %m")
        );
    }
}

void
chdb_channel_prepare_fd(int fd) {
    set_flag(fd, F_GETFD, F_SETFD, FD_CLOEXEC);
    set_flag(fd, F_GETFL, F_SETFL, O_NONBLOCK);
}

/*
 * Sleeps until `fd` is ready, letting a cancel or a shutdown through. While
 * interrupts are held none gets through, so a channel with a hold timeout
 * fails once the waits of one call, timed from the first in `since`, have
 * spent it: a peer that never answers would otherwise hang the backend past
 * pg_cancel_backend, pg_terminate_backend and statement_timeout.
 */
static void
wait_fd(chdbChannel* ch, int fd, uint32 event, instr_time* since) {
    long timeout = CHDB_CHANNEL_POLL_MS;

    if (ch->hold_timeout_ms > 0 && InterruptHoldoffCount > 0) {
        instr_time now;

        INSTR_TIME_SET_CURRENT(now);
        if (INSTR_TIME_IS_ZERO(*since)) {
            *since = now;
        }
        INSTR_TIME_SUBTRACT(now, *since);

        long left = ch->hold_timeout_ms - (long)INSTR_TIME_GET_MILLISEC(now);

        if (left <= 0) {
            ch->fail(ch, ch->wait_what, ETIMEDOUT);
        }
        timeout = Min(timeout, left);
    }
    WaitLatchOrSocket(
        MyLatch,
        event | WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
        fd,
        timeout,
        PG_WAIT_EXTENSION
    );
    ResetLatch(MyLatch);
}

/*
 * Takes whatever the peer has written to its error channel. Keeps the head of
 * it and discards the rest, since a full pipe would stall the peer.
 */
static void
drain_err(chdbChannel* ch) {
    char sink[CHDB_CHANNEL_ERR_MAX];

    while (ch->err >= 0) {
        size_t room = sizeof(ch->err_buf) - 1 - ch->err_len;
        char* into  = room ? ch->err_buf + ch->err_len : sink;
        ssize_t got = read(ch->err, into, room ? room : sizeof(sink));

        if (got > 0) {
            ch->err_len += into == sink ? 0 : (size_t)got;
        } else if (got == 0) {
            chdb_channel_close_fd(&ch->err);
        } else if (errno != EINTR) {
            return; /* EAGAIN, or a pipe we can no longer read */
        }
    }
}

void
chdb_channel_drain_err(chdbChannel* ch) {
    instr_time since = { 0 };

    while (ch->err >= 0) {
        CHECK_FOR_INTERRUPTS();
        drain_err(ch);
        if (ch->err >= 0) {
            wait_fd(ch, ch->err, WL_SOCKET_READABLE, &since);
        }
    }
}

/*
 * The capture stops at whatever byte filled the buffer, so pull the cut back
 * to a character boundary: half a character reaches the client as text and
 * fails its encoding check.
 */
const char*
chdb_channel_error(chdbChannel* ch) {
    /* Strip trailing newlines. */
    while (ch->err_len && (ch->err_buf[ch->err_len - 1] == '\n' ||
                           ch->err_buf[ch->err_len - 1] == '\r')) {
        ch->err_len--;
    }
    ch->err_len =
        (size_t)pg_encoding_mbcliplen(PG_UTF8, ch->err_buf, ch->err_len, ch->err_len);
    ch->err_buf[ch->err_len] = '\0';
    if (!ch->err_len) {
        return NULL;
    }
    ch->err_len = chdb_channel_scrub_error(ch->err_buf, ch->err_len);

    return ch->err_buf;
}

size_t
chdb_channel_scrub_error(char* msg, size_t len) {
    const char* id  = strstr(msg, "Request ID:");
    const char* eol = id ? strchr(id, '\n') : NULL;
    if (eol) {
        memmove((char*)id, eol + 1, strlen(eol + 1) + 1);
        len = strlen(msg);
    }

    const char* version = strstr(msg, " (version ");
    const char* close   = version ? strchr(version, ')') : NULL;
    if (close) {
        memmove((char*)version, close + 1, strlen(close + 1) + 1);
        len = strlen(msg);
    }

    return len;
}

bool
chdb_channel_try_write(chdbChannel* ch, int fd, const void* p, size_t len) {
    const char* at   = p;
    instr_time since = { 0 };

    while (len) {
        CHECK_FOR_INTERRUPTS();
        ssize_t put = write(fd, at, len);

        if (put > 0) {
            at += put;
            len -= put;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            drain_err(ch);
            wait_fd(ch, fd, WL_SOCKET_WRITEABLE, &since);
        } else if (errno != EINTR) {
            return false;
        }
    }

    return true;
}

void
chdb_channel_send_exact(chdbChannel* ch, const void* p, size_t len) {
    if (ch->data < 0 || !chdb_channel_try_write(ch, ch->data, p, len)) {
        ch->fail(ch, ch->send_what, ch->data < 0 ? 0 : errno);
    }
}

/*
 * Reads up to `len` bytes, at least one. Zero is a clean end of stream, and the
 * data descriptor is closed by then.
 */
static size_t
read_some(chdbChannel* ch, void* buf, size_t len) {
    instr_time since = { 0 };

    for (;;) {
        CHECK_FOR_INTERRUPTS();
        if (ch->data < 0) {
            ch->fail(ch, ch->recv_what, 0);
        }

        ssize_t got = read(ch->data, buf, len);
        if (got > 0) {
            return (size_t)got;
        }
        if (got == 0) {
            chdb_channel_close_fd(&ch->data);
            return 0;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            drain_err(ch);
            wait_fd(ch, ch->data, WL_SOCKET_READABLE, &since);
        } else if (errno != EINTR) {
            ch->fail(ch, ch->recv_what, errno);
        }
    }
}

void
chdb_channel_recv_exact(chdbChannel* ch, void* buf, size_t len) {
    char* at = buf;

    while (len) {
        size_t got = read_some(ch, at, len);

        if (got == 0) {
            ch->fail(ch, ch->recv_what, 0);
        }
        at += got;
        len -= got;
    }
}

/* Reads chunk headers until data or the end of the stream. False at the end. */
static bool
next_chunk(chdbChannel* ch) {
    while (ch->chunk_left == 0) {
        if (ch->data_ended) {
            return false;
        }
        chdb_channel_recv_exact(ch, &ch->chunk_left, sizeof(ch->chunk_left));
        if (ch->chunk_left > CHDB_CHUNK_MAX) {
            ch->fail(ch, "bad chunk from chDB", 0);
        }
        ch->data_ended = ch->chunk_left == 0;
    }

    return true;
}

size_t
chdb_channel_recv(chdbChannel* ch, void* buf, size_t len) {
    if (ch->chunked) {
        if (!next_chunk(ch)) {
            return 0;
        }

        size_t got = read_some(ch, buf, Min(len, ch->chunk_left));

        if (got == 0) {
            ch->fail(ch, ch->recv_what, 0);
        }
        ch->chunk_left -= (uint32_t)got;

        return got;
    }

    size_t got = ch->data < 0 ? 0 : read_some(ch, buf, len);

    if (got == 0 && ch->ended) {
        ch->ended(ch);
    }

    return got;
}

void
chdb_channel_write(chdbChannel* ch, const void* p, size_t len) {
    const char* at = p;

    while (len) {
        uint32_t n = ch->chunked ? (uint32_t)Min(len, CHDB_CHUNK_MAX)
                                 : (uint32_t)Min(len, UINT32_MAX);

        if (ch->chunked) {
            chdb_channel_send_exact(ch, &n, sizeof(n));
        }
        chdb_channel_send_exact(ch, at, n);
        at += n;
        len -= n;
    }
}

void
chdb_channel_end_write(chdbChannel* ch) {
    if (ch->chunked) {
        uint32_t zero = 0;

        chdb_channel_send_exact(ch, &zero, sizeof(zero));
    } else {
        chdb_channel_close_fd(&ch->data);
    }
}
