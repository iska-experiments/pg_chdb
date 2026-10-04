/*
 * The engine's read-only mode, for a supervisor on a server in recovery.
 * See readonly.h. The refusals name the reason, which libchdb raises as
 * CALLBACK_OBJECT_STORAGE_ERROR to whatever statement needed the write: a
 * table attached read-only (table_readonly = 1) asks for none, so a
 * refusal in the log is a write the attach or a query should not have
 * made. Merges are stopped for the session as well, in case a table were
 * attached without the setting.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pagecall.h"
#include "readonly.h"
#include "session.h"

static bool readonly;

void
readonly_set(bool ro) {
    readonly = ro;
}

bool
readonly_is(void) {
    return readonly;
}

static int
refuse(void) {
    return pagecall_fail("the store is read-only: the server is in recovery");
}

static int
ro_write_begin(void* ud, const char* key, void** handle) {
    (void)ud;
    (void)key;
    (void)handle;
    return refuse();
}

static int
ro_write_append(void* ud, void* handle, const void* buf, size_t len) {
    (void)ud;
    (void)handle;
    (void)buf;
    (void)len;
    return refuse();
}

static int
ro_write_end(void* ud, void* handle) {
    (void)ud;
    (void)handle;
    return refuse();
}

static int
ro_remove(void* ud, const char* key) {
    (void)ud;
    (void)key;
    return refuse();
}

static int
ro_copy(void* ud, const char* from, const char* to) {
    (void)ud;
    (void)from;
    (void)to;
    return refuse();
}

void
readonly_callbacks(chdb_object_storage_callbacks* cb) {
    if (!readonly) {
        return;
    }
    cb->write_begin  = ro_write_begin;
    cb->write_append = ro_write_append;
    cb->write_commit = ro_write_end;
    cb->write_abort  = ro_write_end;
    cb->remove       = ro_remove;
    cb->copy         = ro_copy;
}

bool
readonly_session(void) {
    static const char sql[] = "SYSTEM STOP MERGES";
    char* err               = session_run(sql, sizeof(sql) - 1);

    if (err) {
        fprintf(stderr, "chdb_search_engine: could not stop merges: %s\n", err);
        free(err);
        return false;
    }
    return true;
}
