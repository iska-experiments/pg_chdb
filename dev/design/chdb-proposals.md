# Proposals for chdb-core

Two changes to libchdb that pg_chdb's search indexes need. The first is
implemented on the `callback-object-storage` branch of chdb-core; the second
is a request for discussion.

## 1. Host-provided object storage (callback disk)

### Problem

A Postgres extension that keeps ClickHouse MergeTree data for an index must
make that data crash-safe and replicable the way Postgres users expect. The
only mechanism Postgres gives an extension is its own buffer manager and WAL:
bytes written into relation pages through `GenericXLog` survive crashes and
reach physical standbys. Files chDB writes under `--path` do neither.

### Proposal

Add an object storage type whose blob operations are C callbacks supplied by
the embedding process before `chdb_connect`:

```c
typedef struct chdb_object_storage_callbacks {
    uint32_t struct_size;          /* sizeof, for forward compatibility */
    void *ud;
    int (*exists)(void *ud, const char *key);
    int (*metadata)(void *ud, const char *key, uint64_t *size, int64_t *mtime);
    int (*read)(void *ud, const char *key, uint64_t offset, void *buf, size_t len, size_t *out);
    int (*write_begin)(void *ud, const char *key, void **handle);
    int (*write_append)(void *ud, void *handle, const void *buf, size_t len);
    int (*write_commit)(void *ud, void *handle);
    int (*write_abort)(void *ud, void *handle);
    int (*remove)(void *ud, const char *key);
    int (*list)(void *ud, const char *prefix,
                void (*sink)(void *sink_ud, const char *key, uint64_t size, int64_t mtime),
                void *sink_ud);
    const char *(*last_error)(void *ud);
} chdb_object_storage_callbacks;

chdb_state chdb_register_object_storage(const char *name, const chdb_object_storage_callbacks *cb);
chdb_state chdb_unregister_object_storage(const char *name);
```

```sql
CREATE TABLE t (...) ENGINE = MergeTree ORDER BY k
SETTINGS disk = disk(type = 'callback', storage_name = 'pg_16401');
```

The metadata layer is ClickHouse's existing `plain_rewritable`, so the host
supplies only blobs and gets directories, renames and listing from code that
already exists. The implementation is an `IObjectStorage` modelled on
`LocalObjectStorage` without its logging decorators, plus a new
`ObjectStorageType::Callback`. It mirrors how `chdb_arrow_scan` already hands
host callbacks (Arrow C streams) into the engine through a process-global
registry.

### Why not alternatives

* A FUSE or 9p mount is not available inside a database server process.
* Copying part files into pages after each insert doubles storage and makes
  recovery a restore instead of a replay.
* A full `IDisk` would have to re-implement hardlinks, transactions and
  renames that the metadata layer already provides.

### Open points

* `plain_rewritable` has no hardlinks, so mutations that rely on them need
  the same fallback ClickHouse uses for plain disks today. We list which
  operations that affects in the PR.
* Thread safety: callbacks are invoked from ClickHouse threads. The Postgres
  host serialises them onto its main thread through a queue; the API should
  state that callbacks may be called concurrently so hosts know to do so.

## 2. Multi-process read access to one store path

### Problem

`chdb_connect("--path=X")` takes `X/status` with an exclusive lock, so a second
process cannot open the same store even read-only. Postgres is one process
per connection. Today every backend must forward queries to the one process
that holds the store, which adds a hop and makes that process a bottleneck
for read scaling.

### Proposal for discussion

A read-only attach mode, for example `--path=X --readonly=1`, that:

* does not take the status lock, and refuses DDL and DML;
* loads MergeTree parts from the directory and watches for new or removed
  parts (the owner writes parts atomically, so readers can pick them up by
  listing, the way `clickhouse-local` with a web disk does);
* exposes an API to refresh the parts list (`chdb_refresh(conn)`) so the host
  can call it after it knows a commit landed.

With the callback disk from proposal 1 the same shape applies: readers see
the pages the owner committed, and Postgres already provides the
cross-process visibility.

### Why it matters

Postgres users scale reads by adding backends; a per-database worker that
serialises all searches undoes that. Read-only attach would let each backend
run its own searches against committed parts while the worker only ingests.

## Status and follow-ups (2026-10-03)

Proposal 1 is implemented on chdb-core branch `callback-object-storage`
(iskakaushik/chdb-core) and open as a draft PR against chdb-io/chdb-core.
The review of that branch left these follow-ups for the chDB team, in the
order we need them:

1. An optional idle hook on the thread inside `chdb_query`, so a
   single-threaded host can serve callbacks itself. pg_chdb does not need it
   because libchdb runs in the engine child, but other hosts will.
2. Fail fast when a callback re-enters `chdb_query` (today: deadlock or UB).
3. Fence callbacks after `chdb_unregister_object_storage` so a host that has
   torn down its state can stop late calls from background merges.
4. Late-bind unregistered callback disks at attach, removing the
   register-before-reopen rule.
5. `chdb_shutdown()` returns an error after any MergeTree insert because
   `GlobalThreadPool::shutdownAndJoin` finds a parked thread; pre-existing.
6. Upstream ClickHouse: a `__trash/` prefix for `removeRecursive` on plain
   rewritable disks so interrupted removals can be swept.

Proposal 2 (read-only attach) is unchanged and still a request for
discussion.

## What pg_chdb's CI needs (2026-10-04)

The search engine now keeps its store on the callback disk, so it builds
and runs only against a libchdb that has `chdb_register_object_storage`,
and no release has it. pg_chdb's CI therefore needs the chdb-core build
with the callback disk (chdb-io/chdb-core#256) published as a release, or
at least as a downloadable tarball of `include/chdb.h` and
`lib/libchdb.so` for linux-amd64. Until then CI builds without the engine
(`make CHDB_SEARCH_ENGINE=`) and tests the search module with the stub
worker client only, with a warning saying so; a repository variable
`LIBCHDB_CALLBACK_URL` naming such a tarball turns the legs that need the
engine back on. The worker's SQL and TAP tests run locally against a
chdb-core build in the meantime.
