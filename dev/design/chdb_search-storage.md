# chdb_search storage: store layout, recovery and replication

The storage half of the `chdb_search` contract in `chdb_search.md`: how the
chDB store sits beside the index relation and proves itself current, what
the Phase 1 callback disk asks of the worker, and what the index
guarantees under backups and replication. Keep it current with the other
half when an interface changes.

## Storage and recovery

Phase 0 (local directory, gone): the index relation had one metapage
holding a magic, version, a random 64-bit store generation and the store's
LSN high-water mark written at every flush, and the store kept a `meta`
table of the same so that a backend could refuse a store from another
point in time than its index: a restore, a `pg_rewind`, a crash between a
flush and its commit.

Phase 1 (callback disk, chdb-core branch `callback-object-storage`, open
as chdb-io/chdb-core#256; today): chdb-core gained a `callback` object
storage type and the registration

```c
typedef struct chdb_object_storage_callbacks {
    uint32_t struct_size;
    void *ud;
    int (*exists)(void *ud, const char *key, int *out);
    int (*metadata)(void *ud, const char *key, int *found, uint64_t *size, int64_t *mtime);
    int (*read)(void *ud, const char *key, uint64_t offset, void *buf, size_t len, size_t *out);
    int (*write_begin)(void *ud, const char *key, void **handle);
    int (*write_append)(void *ud, void *handle, const void *buf, size_t len);
    int (*write_commit)(void *ud, void *handle);
    int (*write_abort)(void *ud, void *handle);
    int (*remove)(void *ud, const char *key);
    int (*list)(void *ud, const char *prefix, chdb_object_storage_list_sink sink, void *sink_ud);
    int (*copy)(void *ud, const char *from_key, const char *to_key);   /* optional */
    const char *(*last_error)(void *ud);                                /* optional */
} chdb_object_storage_callbacks;
chdb_state chdb_register_object_storage(const char *name, const chdb_object_storage_callbacks *cb);
```

with `SETTINGS disk = disk(type = 'callback', storage_name = 'pg_<indexoid>',
key_prefix = 'g<generation>')` on the store tables (`CHDB_STORE_DISK_FMT`),
metadata `plain_rewritable`. The engine's callbacks cross the page
socketpair to the worker (`pagestore/protocol.h`), which answers them from
the pages of the index relation behind `chdbBlobStore` (`pagestore/store.h`,
`pagebackend.c`). Stage 1, a directory of files behind the same table,
proved the protocol and is gone.

**The page format** (`pagestore/pages.h`). Block 0 is the metapage: the
generation and `flushed_lsn` the access method keeps, the heads of two
chains, and a dirty flag. The directory is a chain of pages of
`ChdbDirEntry` items, unsorted and read whole for a key (a few pages in
shared buffers against a socket round trip per callback): each maps a key
to its size, its commit time and either the blob's bytes, when they fit
beside the key (`CHDB_INLINE_MAX`, 2 kB, which takes most of a part's
small files), or the first page of a map chain listing the blob's data
pages in order, so that a read at an offset reaches its page in as many
hops as there are map pages before it (2038 pages, 16 MB, per map page).
Data pages hold `CHDB_DATA_PER_PAGE` (8160) bytes. The free stack is a
chain of pages listing free pages by number: a page that runs out of
numbers is itself the next one handed out, and the first page of a run
being freed becomes the new top when the top is full, so freeing
allocates nothing and a 1 GB blob is freed in 64 records. Every write is
one generic WAL record over at most four buffers under exclusive buffer
locks (`ChdbWrite`); a blob of several pages is published by its directory
entry, written last, after its data and map pages. A copy, which
plain_rewritable makes of every blob before it unlinks a part, shares the
source's chain by a count on its head.

**Locators, not the relcache.** A `CREATE INDEX` or `REINDEX` streams its
rows while the relation it writes is visible to its own backend alone, so
the worker cannot open it by OID. Every request frame names the relation
as the backend sees it (tablespace OID and relfilenumber); the worker reads
the generation off its metapage and routes the keys under that generation's
prefix to it (`pagestore/routes.c`), reading buffers with
`ReadBufferWithoutRelcache` and logging them through a stand-in relation
that answers only `RelationNeedsWAL`. A generation never noted, or whose
relation Postgres has unlinked, is an empty storage: nothing is found in
it, and removing from or copying within it succeeds, which is what the
engine's drop of such a table needs (ClickHouse retries a failing drop
without end, and `DROP ... SYNC` waits on it). Blobs are served outside any
transaction, under a resource owner of the store's own (`pagestore/owner.c`).

**Recovery of leaks.** A crash between a write's pages and its entry leaves
pages no entry names, and one between a copy's count and its entry a count
too high. The metapage's dirty flag is set by the first allocation a worker
makes in a relation and cleared when that worker stops with nothing
pending; a worker that first sees a relation with the flag set walks the
directory and the free stack, returns every page neither reaches to the
stack, and puts the counts right (`pagestore/recover.c`), before serving a
blob from it.

**The engine's directory is a cache.** `$PGDATA/pg_chdb/pgsql_tmp/<dboid>/`
holds chDB's metadata and scratch space; the worker empties it when it
starts (a crash, a restore, a rewind or a promotion may have left it older
or newer than the pages) and attaches each index's table from the catalog
on the first request naming the index (`attach.c`): the table's UUID is
fixed by the index OID and generation (`ddl.c`), so `ATTACH TABLE IF NOT
EXISTS` finds the parts under the generation's key prefix. Staging tables
have a prefix of their own, `s<generation>_tx_<fxid>`, and are attached
from the prefixes the blobs carry, so a transaction mid-flight survives a
worker restart and a crashed one's table is still there for VACUUM to
sweep; commit's `ATTACH PARTITION tuple() FROM` copies a staging table's
parts from its prefix to the table's on the one storage. A generation
whose table has written no blob yet is not attached: its `CREATE TABLE`
is still to come, as a `CREATE INDEX CONCURRENTLY`'s is between its
catalog entry and its build. The directory is named for
Postgres to leave out of base backups and `pg_rewind`, as it does every
`pgsql_tmp`. The `meta` table and the LSN comparison are gone: the store
and the index are one relation.

**Rows of transactions that never committed.** A flush sends a
transaction's rows before its commit record, so a crash in between leaves
rows in the store whose heap tuples are dead, and a pruned tuple's TID is
reused. Every store row carries its inserting transaction id (`xmin`); a
scan skips, and VACUUM deletes, the rows whose transaction is known to have
ended without committing (`xmin.c`), judged against the commit log inside
the window between the heap's `relfrozenxid` and the next transaction id,
and taken as committed outside it. This replaces the LSN comparison for
the one case it caught that recovery does not: it is exact per row, and a
failed commit after a flush is covered as well.

## Phase 1 host contract (from the chdb-core review)

The review of the callback disk fixed the host-side rules the worker must
honour, and the supervisor/engine split turns out to be what makes them
satisfiable:

* **Threads.** The engine calls storage callbacks from pool and merge
  threads while the thread inside `chdb_query` is blocked, so a callback
  may never call back into libchdb or wait for the thread that is inside
  it. A single-threaded Postgres process cannot both run `chdb_query` and
  serve its own callbacks. Therefore libchdb runs only in the engine child
  (`src/search/engine/`), whose callbacks forward page requests over the
  socketpair, and the supervisor bgworker, which is never inside libchdb,
  serves them from its event loop and from inside every wait on the request
  channel. chdb-core follow-up 25 (an idle hook on the calling thread)
  would remove the need for the split for other hosts; we do not depend on it.
* **Durability.** `write_commit` is the only durability point the engine
  exercises (the disk reports remote, so MergeTree never fsyncs), and the
  contract allows durability in commit order. The supervisor writes a
  blob's pages and its directory entry as generic WAL records and flushes
  nothing: the commit record of the Postgres transaction behind the flush
  follows them in the WAL, and `COMMIT` flushes up to it. A failed
  `write_commit` releases the handle and no abort follows, so the supervisor
  frees the pages itself on failure. `write_begin` can be called for a key
  that already exists (rewrite), and zero-length blobs are legal.
* **Removal must not fail.** plain_rewritable unlinks a blob by copying it
  aside, removing it and removing the copy, and ClickHouse drops a table's
  data in a background task it retries without end while `DROP ... SYNC`
  waits. Every callback on that path returns success for a storage whose
  relation is gone.
* **Process exit.** An open libchdb connection at `exit()` tears the engine
  down from an atexit handler, which fires `write_abort`/`remove`
  callbacks. The engine closes its connection before exiting on SIGTERM;
  the supervisor closes its socket in `before_shmem_exit`, not after shared
  memory is gone.
* **Registration order.** Callbacks must be registered before the
  `chdb_connect` that reopens a path holding tables on the disk, because
  metadata load instantiates the disk at attach. The engine asks the
  worker for its storages first; after a worker restart there are none, as
  the directory was emptied, and the tables are attached on demand.
* **Mutations.** `plain_rewritable` has no hard links, so VACUUM's deletes
  use `lightweight_delete_mode = 'lightweight_update_force'` and the store
  table enables block number and offset columns.

## Backups and replication

The index behaves like any other Postgres index under base backups,
point-in-time recovery, streaming replication and `pg_rewind`: the pages
are its store, and generic WAL (`RM_GENERIC`) is replayed by core with no
resource manager of our own and no `shared_preload_libraries`.

| Property | Phase 0 (directory under PGDATA) | Today (Phase 1, parts in index pages) |
|---|---|---|
| Base backup + PITR | Heap restored to the target; store a copy from backup time, so the index is **stale and must be rebuilt** | Consistent at the target LSN, no rebuild |
| WAL-G delta backups | Store files have no page LSNs, so every delta copies the whole store | Standard pages, delta works; the engine's cache directory is excluded as `pgsql_tmp` |
| Streaming standby | No store on the standby; the index is **unusable** until promotion and rebuild | The pages are replicated and a worker on the standby serves searches from them, its engine read-only; a promoted standby serves at once, read-write, no rebuild |
| `pg_rewind` | Store copied wholesale, then treated as stale | Rewound with the other relation files |
| Logical replication | Works: the subscriber maintains its own index through `aminsert` | Same |
| Replay requirements | None | None: generic WAL, replayed by core |

Fail-safe rule: a scan never returns rows from a store it cannot prove
current. Today that proof is the relation itself, on a standby as on
the primary; what the access method still checks before a scan, a flush
and VACUUM's deletes is that the pages hold a store at all, any blob under
the generation's prefix, and `chdb_search.unavailable_index = error |
skip` decides what to do with an index that has none: `error` raises
"chdb index has no store", `skip` makes `amcostestimate` return
`disable_cost` so the planner uses another path. The default is `error`,
because a silent fallback to a sequential scan hides an index that is not
serving. An index whose build finished always has a store, as MergeTree
writes a file into a table's directory as it creates it.

**Standby reads** (`standby.c`, `engine/readonly.c`). The worker starts on
a hot standby too (`BgWorkerStart_ConsistentState`) and serves searches
from the replayed pages. Its engine is started with the `readonly`
argument: every callback that would write refuses with "the store is
read-only: the server is in recovery", libchdb raises the refusal to the
statement that needed the write, and the session runs `SYSTEM STOP
MERGES`; the worker refuses the same page requests itself, behind the
engine. Tables are attached with `table_readonly = 1`, which makes
MergeTree write nothing and schedule no background work, and without the
staging tables, which are the primary's transactions'. Attaching from
the listing reads only, as the pages hold everything MergeTree creates
with a table (`format_version.txt`, the `detached` directory's marker),
so nothing is served from an overlay. Replay changes the parts under the
engine, so every request on a standby reads a version of the index's
blobs, the latest LSN among its metapage and directory pages, and a
request that finds it moved detaches the table and attaches it again from
what the pages hold now: a search on a standby is current to the last
replayed record, at the cost of a reattach after each flush or merge the
primary makes. The dirty flag a primary's worker leaves on a relation is
not recovered on the standby, which cannot write, and its routes are not
cleaned when the worker stops.

Promotion: the event loop waits at most a second while the server is in
recovery and, when `RecoveryInProgress()` turns false, stops the engine,
empties its directory, a cache of tables attached read-only, and forgets
the relations and tables noted, so the next request starts a read-write
engine and attaches afresh, recovering any relation a crashed primary left
dirty. Nothing is rebuilt.

## WAL volume

MergeTree merges rewrite parts, and every rewritten byte is a generic WAL
full-page image, so the store's WAL is its bytes written, plus the merges.
`dev/benchmark/pagestore.sql` measures it from `pg_current_wal_lsn()`
deltas: the heap alone, the build, one transaction of many rows, single-row
commits (each a flush and a part) and `OPTIMIZE TABLE ... FINAL`, with the
same script at the stage-1 commit for the share that is the store's.

WAL_NUMBERS_PLACEHOLDER

Two-phase commit: the buffer flushes at `XACT_EVENT_PRE_PREPARE` as it does
at pre-commit. `ROLLBACK PREPARED` then leaves rows in the store whose heap
tuples are dead; the xmin check hides them and VACUUM removes them.

WAL-G's `backup-push` tars every file under the data directory, and tar has
no entry for a socket, so it failed while a worker listened on
`pg_chdb/<dboid>.sock`, where `pg_basebackup` skips the socket with a
warning. On Linux the worker now listens in the abstract namespace, on
`@pg_chdb/<hash>/<dboid>` with the hash of the data directory's path,
device and inode, so the data directory holds no socket and a backup runs
with the workers up; `SO_PEERCRED` stands in for the file mode, admitting
peers of the server's uid only. Elsewhere the socket is still a file, now
`pg_chdb/pgsql_tmp/<dboid>.sock`, which WAL-G leaves out with every
`pgsql_tmp` as `pg_basebackup` does, so a backup runs with the workers up
there too.

Tests: `t/search_standby.pl` (a streaming standby answers a search from
the replicated pages, sees rows replayed after its first search, writes
nothing, and once promoted serves and takes rows with no REINDEX; the
cache directory removed and rebuilt), `t/search_crash.pl` (a backend and the postmaster killed with
rows in flight; recovery leaves the index consistent with the heap),
`t/search_pagestore.pl` (an engine killed inside a blob; freed pages
reused; DROP INDEX takes the pages), `t/search_pitr.pl` (base backup with
WAL archiving restored to a PITR target), `t/search_logical.pl` (a
logical subscription whose subscriber builds its own index),
`t/search_walg.pl` (WAL-G itself with `WALG_FILE_PREFIX` pointing at a
local directory, `backup-push`, `wal-push`, `backup-fetch` and
`wal-fetch`, skipped when the binary is absent, backing up with the worker
running) and `t/search_twophase.pl` (two-phase commit through the store).
