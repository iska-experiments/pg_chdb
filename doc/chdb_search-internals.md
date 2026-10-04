chdb_search Internals
=====================

How the [chdb_search](chdb_search.md) extension is put together, for
operators who need to know what a failure does and for contributors. The
authoritative contract is the [design notes][design]; this page summarizes
the parts users can observe. Parts marked **Requires the access method**
describe the index access method, which is not yet in the tree.

## How It Works

```
 backend (chdb_search.so)                 chdb_search worker (per database)
 ┌──────────────────────────┐   unix      ┌────────────────────────────────┐
 │ index AM  (amhandler)    │  socket     │ accept loop, one thread/conn   │
 │ insert buffer → precommit├────────────►│ libchdb session  --path=STORE  │
 │ scan: ctid stream        │◄────────────┤ Native blocks in/out           │
 │ CustomScan / agg pushdown│             │ pg_pages disk callbacks        │
 └──────────────────────────┘             │   (Phase 1) → shared buffers   │
                                          └────────────────────────────────┘
```

chDB takes an exclusive lock on its store path, so only one process can use a
store. Rather than have each backend fork a helper, as `chdb` does for
one-off queries, each database gets one background worker that links libchdb
and holds the store open. Backends never load libchdb. They send requests
over a Unix stream socket and receive results as ClickHouse Native blocks.

The index maps to one ClickHouse MergeTree table per index, `t` in the chDB
database `idx_<index oid>`, ordered by the heap tuple id (`ctid`, packed into
a `UInt64`). Text columns carry a ClickHouse `text` skip index. A query
selects the matching `ctid`s from the worker, and the executor fetches those
tuples from the heap and rechecks visibility.

**Requires the access method** for everything beyond the worker: the buffer
that collects a transaction's rows, the flush at `XACT_EVENT_PRE_COMMIT`, the
scans (`amgettuple` and `amgetbitmap`), and the planner paths.

## The Worker

*   **Start.** The first backend in a database that needs a worker
    registers it with `RegisterDynamicBackgroundWorker` and waits for it to
    listen. A small registry in dynamic shared memory records one slot per
    database, so concurrent backends start only one. The registry holds 64
    databases.
*   **Place.** The socket is `$PGDATA/pg_chdb/<database oid>.sock` and the
    store `$PGDATA/pg_chdb/<database oid>/`, one chDB database per index.
*   **Protocol.** One request at a time per connection: a command (`SELECT`,
    `INSERT`, `EXEC`, or `DROP`), the index OID, the query, and for inserts
    a stream of Native blocks, answered by a status frame. A failed query
    reports its error and leaves the worker serving.
*   **Restart.** The postmaster restarts a dead worker after five seconds,
    and the next backend that needs one may register it sooner. A restarted
    worker unlinks its predecessor's socket and reopens the same store.

### What a Crash Does

The worker is a background worker with shared memory access, so the
postmaster cannot tell a worker's death by a signal from a backend's: it is
treated as a crash of the instance. When libchdb faults, or the worker is
killed with `SIGKILL` or `SIGSEGV`, the postmaster logs:

```
LOG:  background worker "chdb_search worker" (PID 2597755) was terminated by signal 11: Segmentation fault
LOG:  terminating any other active server processes
LOG:  all server processes terminated; reinitializing
LOG:  database system was not properly shut down; automatic recovery in progress
```

Every session is disconnected, crash recovery runs, and the instance accepts
connections again. The next call starts a new worker over the same store and
sees the data written before. This differs from `chdb_helper`, which holds no
shared memory and costs only the one `COPY` that was using it.
`t/search_worker.pl` checks both signals.

With `restart_after_crash = off`, the instance stops instead. A clean
termination, such as `pg_terminate_backend()` on the worker pid, is not a
crash: the worker logs that it is shutting down, and the next call starts
another.

> [!WARNING]
> `DROP DATABASE` fails with `database "name" is being accessed by other
> users` while that database's worker runs, because nothing stops the worker
> first. Terminate it with `pg_terminate_backend()` and retry before the
> postmaster registers its replacement.

## Consistency

**Requires the access method.** Rows are buffered per backend, per index, and
shipped as one Native block at pre-commit, so the commit waits for the
worker's acknowledgment. Nothing reaches the store from a transaction that
aborts, and a transaction larger than `chdb_search.flush_threshold` flushes
early into a staging table that is merged at pre-commit and dropped on abort.

A crash between the flush and the commit leaves rows in the store whose
transaction never committed. They are harmless: the heap recheck discards
tuple ids that are not visible, and `VACUUM` removes them. On open, the
worker compares a generation and LSN high-water mark kept in the index
metapage with the store's, and marks the index invalid when they differ, as
after a restore from backup.

## Storage

*   **Phase 0** (this tree): a local directory under `$PGDATA/pg_chdb`.
    It is not WAL-logged and is not copied by physical replication.
*   **Phase 1**: a chDB disk whose blobs live in index relation pages written
    with generic WAL by the worker, upstreamed to chDB as a callback object
    storage. Crash recovery and physical replication then come from Postgres.

## Limitations

*   chDB allows one process per store path, so reads serialize through the
    worker until chDB supports read-only multi-process attach.
*   No physical replication in Phase 0.
*   The Postgres-side fallback implementations match only the default
    tokenizer, `splitByNonAlpha` with lowercasing.
*   BM25 and relevance scores are out of scope: the index filters.

  [design]: ../dev/design/chdb_search.md "chdb_search design notes"
