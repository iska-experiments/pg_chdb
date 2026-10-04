#ifndef CHDB_CHANNEL_H
#define CHDB_CHANNEL_H

#include "postgres.h"

/*
 * A duplex byte channel to a process that runs chDB, with an optional pipe for
 * its error text. Two ways open one: helper.c forks chdb_helper for a COPY,
 * and the chdb_search client connects to the worker's socket. Past that, both
 * trade Native blocks the same way, so native.c takes a channel and neither
 * side carries its own copy of the waiting, interrupt and cleanup code.
 *
 * The descriptors are nonblocking. Every wait is on the latch, so a cancel or
 * a shutdown gets through, and the error pipe is drained whenever the data
 * channel would block, since a full pipe stalls the peer.
 *
 * A channel is either plain, where end of stream is end of data, or chunked:
 * uint32 byte count and that many bytes, ended by a zero count. The chunked
 * form lets one connection carry many requests.
 */

#define CHDB_CHANNEL_ERR_MAX 4096

/* Largest chunk either side will take, so a corrupt count cannot size a buffer. */
#define CHDB_CHANNEL_CHUNK_MAX (8 * 1024 * 1024)

typedef struct chdbChannel chdbChannel;

struct chdbChannel {
    MemoryContextCallback cleanup;
    int data; /* -1 once closed */
    int err;  /* error pipe read end, -1 at EOF or when there is none */
    bool chunked;
    uint32_t chunk_left; /* bytes of the current inbound chunk not yet read */
    bool data_ended;     /* the zero chunk has been read */

    /* What went wrong, for the message when a read or write breaks. */
    const char* recv_what;
    const char* send_what;

    /* Raises. `errnum` is zero for a peer that simply went away. */
    void (*fail)(chdbChannel* ch, const char* what, int errnum);
    /* Plain channels, at end of data: raises if the peer ended badly. Optional. */
    void (*ended)(chdbChannel* ch);
    /* Memory context reset, once the descriptors are closed. Optional. */
    void (*stop)(chdbChannel* ch);

    size_t err_len;
    char err_buf[CHDB_CHANNEL_ERR_MAX];
};

/* Starts a channel on `data`, which it will close, and `err` or -1. */
extern void
chdb_channel_init(chdbChannel* ch, int data, int err);

/* Makes the current memory context close the channel when it is reset or deleted. */
extern void
chdb_channel_own(chdbChannel* ch);

extern void
chdb_channel_close(chdbChannel* ch);

/* Marks a descriptor close-on-exec and nonblocking, raising if it cannot. */
extern void
chdb_channel_prepare_fd(int fd);

/* Closes `*fd` if open and sets it to -1. */
extern void
chdb_channel_close_fd(int* fd);

/*
 * Reads up to `len` bytes of data, returning zero at the end of the stream.
 * On a plain channel the end is the peer closing; on a chunked one it is the
 * zero chunk, after which the channel is still open for the status frame.
 */
extern size_t
chdb_channel_recv(chdbChannel* ch, void* buf, size_t len);

/* Sends data, as chunks on a chunked channel. */
extern void
chdb_channel_write(chdbChannel* ch, const void* p, size_t len);

/* Ends the data: the zero chunk on a chunked channel, else closes our end. */
extern void
chdb_channel_end_write(chdbChannel* ch);

/* Reads or writes exactly `len` bytes outside the chunks. */
extern void
chdb_channel_recv_exact(chdbChannel* ch, void* buf, size_t len);

extern void
chdb_channel_send_exact(chdbChannel* ch, const void* p, size_t len);

/* Writes to any descriptor, draining errors while it blocks. False, errno set, if it
 * fails. */
extern bool
chdb_channel_try_write(chdbChannel* ch, int fd, const void* p, size_t len);

/* Takes the peer's error output until its end, waiting for it. */
extern void
chdb_channel_drain_err(chdbChannel* ch);

/*
 * The peer's error text, minus a Request ID line and chDB version that differ
 * between runs, cut on a character boundary. NULL when it said nothing.
 */
extern const char*
chdb_channel_error(chdbChannel* ch);

/*
 * Drops what differs between runs of one chDB error from the NUL-terminated
 * `msg` of `len` bytes: the Request ID line and the version suffix. Returns the
 * new length. For error text that arrived whole, outside a channel's capture.
 */
extern size_t
chdb_channel_scrub_error(char* msg, size_t len);

#endif /* CHDB_CHANNEL_H */
