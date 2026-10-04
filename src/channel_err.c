/*
 * The error text a chDB process wrote: cut to a character boundary, and
 * scrubbed of what differs between runs of the same failure. See channel.h.
 */

#include "postgres.h"

#include "mb/pg_wchar.h"

#include "channel.h"

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
