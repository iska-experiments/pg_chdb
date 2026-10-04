# chdb_search storage: store layout, recovery and replication

The storage half of the `chdb_search` contract in `chdb_search.md`: how the
chDB store sits beside the index relation and proves itself current, what
the Phase 1 `pg_pages` disk asks of the worker, and what the index
guarantees under backups and replication. Keep it current with the other
half when an interface changes.

## Storage and recovery

Phase 0 (local directory): the index relation has one metapage (WAL-logged)
holding a magic, version, a random 64-bit store generation, and the store's
LSN high-water mark written at every flush. Beside its generation tables
the store keeps a `meta` table, (generation, lsn), which the engine creates
with the database (`CHDB_STORE_META_DDL`) and the access method writes at
every flush; a backend checks its `max(lsn)` against the metapage before
trusting the store; mismatch (crash between flush and commit, restore from backup,
`pg_rewind`) marks the index invalid (`indisvalid = false`) and schedules a
rebuild. `DROP INDEX` and `REINDEX` register the store directory for
removal in an `XACT_EVENT_COMMIT` callback; before it listens, the worker
sweeps the `idx_*` databases whose OID is not a chdb index (`sweep.c`), and
the `pg_chdb/<dboid>` directories of databases no longer in `pg_database`,
which is how a drop made without the library loaded is cleaned up.

Phase 1 (`pg_pages` disk, chdb-core PR): chdb-core gains

```c
typedef struct chdb_object_storage_callbacks {
    void *ud;
    int  (*exists)(void *ud, const char *key);
    int  (*read)(void *ud, const char *key, uint64_t offset, void *buf, size_t len, size_t *out);
    int  (*write_begin)(void *ud, const char *key, void **handle);
    int  (*write_append)(void *ud, void *handle, const void *buf, size_t len);
    int  (*write_commit)(void *ud, void *handle);
    int  (*write_abort)(void *ud, void *handle);
    int  (*remove)(void *ud, const char *key);
    int  (*list)(void *ud, const char *prefix, chdb_list_sink sink, void *sink_ud);
    int  (*metadata)(void *ud, const char *key, uint64_t *size, int64_t *mtime);
} chdb_object_storage_callbacks;
chdb_state chdb_register_object_storage(const char *name, const chdb_object_storage_callbacks *cb);
```

and a `callback` object storage type (`SETTINGS disk = disk(type='object_storage',
object_storage_type='callback', metadata_type='plain_rewritable', name='pg_<indexoid>')`).
The worker implements the callbacks on index-relation pages: a block
directory in pages 1..n maps keys to page chains; blob pages are written
with `GenericXLogStart/RegisterBuffer/Finish` under `BUFFER_LOCK_EXCLUSIVE`.
Then recovery and physical replication come from Postgres WAL, the
generation check becomes a sanity check, and the local directory holds only
chDB's own `tmp/` and `metadata/`, which are rebuilt from pages on start.

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
  serves them from its event loop. Page requests are interleaved with the
  response stream, so the relay answers some frames instead of forwarding
  them. chdb-core follow-up 25 (an idle hook on the calling thread) would
  remove the need for the split for other hosts; we do not depend on it.
* **Durability.** `write_commit` is the only durability point the engine
  exercises (the disk reports remote, so MergeTree never fsyncs). The
  supervisor writes a blob's pages and its directory entry with
  `GenericXLogFinish` before acknowledging the commit; WAL ordering then
  gives durability in commit order. A failed `write_commit` releases the
  handle and no abort follows, so the supervisor must drop the staged pages
  itself on failure. `write_begin` can be called for a key that already
  exists (rewrite), and zero-length blobs are legal.
* **Process exit.** An open libchdb connection at `exit()` tears the engine
  down from an atexit handler, which fires `write_abort`/`remove`
  callbacks. The engine closes its connection before exiting on SIGTERM;
  the supervisor closes its socket in `before_shmem_exit`, not after shared
  memory is gone.
* **Registration order.** Callbacks must be registered before the
  `chdb_connect` that reopens a path holding tables on the disk, because
  metadata load instantiates the disk at attach.
* **Mutations.** `plain_rewritable` has no hard links, so VACUUM's deletes
  use `lightweight_delete_mode = 'lightweight_update_force'` and the store
  table enables block number and offset columns.

## Backups and replication

The index must behave like any other Postgres index under WAL-G backups,
point-in-time recovery, streaming replication, `pg_rewind` and logical
replication. The two storage phases differ, and the guarantees are explicit.

| Property | Phase 0 (directory under PGDATA) | Phase 1 (parts in index pages) |
|---|---|---|
| Base backup + PITR | Heap restored to the target; store is a copy from backup time, so the index is **stale and must be rebuilt** | Consistent at the target LSN, no rebuild |
| WAL-G delta backups | Store files have no page LSNs, so every delta copies the whole store | Standard pages, delta works |
| Streaming standby | No store on the standby; the index is **unusable** until promotion and rebuild | Standby worker opens the store read-only from pages and serves searches |
| `pg_rewind` | Store copied wholesale, then treated as stale | Rewound with the other relation files |
| Logical replication | Works: the subscriber maintains its own index through `aminsert` | Same |
| Replay requirements | None | None: generic WAL (`RM_GENERIC`) is replayed by core, no custom rmgr, no `shared_preload_libraries` |

Fail-safe rule for both phases: a scan never returns rows from a store it
cannot prove current. At scan start the AM compares the metapage generation
and last-flushed LSN with what the store reports (`SELECT max(lsn)` on a
one-row `meta` table in the store, written with every flush). On mismatch,
on a missing store, or on a standby in Phase 0, behaviour follows
`chdb_search.unavailable_index = error | skip`: `error` raises
"chdb index is not available on this server, REINDEX to rebuild", `skip`
makes `amcostestimate` return `disable_cost` so the planner uses another
path. The default is `error`, because a silent fallback to a sequential
scan hides a broken index. REINDEX always rebuilds the store from the heap.

Phase 1 standby reads: the standby's worker opens the index pages
read-only. The ClickHouse table definition is regenerated from the Postgres
index definition (the same `ddl.c` code that created it), the parts are
discovered by listing the page directory as a `plain_rewritable` disk does,
and the engine runs with `SYSTEM STOP MERGES` and a read-only disk so it
never writes. Promotion switches the worker to read-write without a
rebuild.

Write amplification: MergeTree merges rewrite parts, and in Phase 1 every
rewritten byte is a generic WAL full-page image. Keep parts few and large
(`min_bytes_for_wide_part`, merge settings on the store table), materialize
skip indexes once after bulk loads rather than on every merge
(`materialize_skip_indexes_on_merge = 0` plus `MATERIALIZE INDEX`), and
document the expected WAL volume per inserted row in `doc/chdb_search.md`.

Two-phase commit: the buffer flushes at `XACT_EVENT_PRE_PREPARE` as it does
at pre-commit. `ROLLBACK PREPARED` then leaves rows in the store whose heap
tuples are dead; the visibility recheck hides them and VACUUM removes them.

Tests (`t/replication.pl`, `t/backup.pl`): base backup with WAL archiving
restored to a PITR target; a streaming standby queried through the index
(Phase 0 asserts the fail-safe error, Phase 1 asserts rows); a logical
subscription whose subscriber builds its own index; and WAL-G itself with
`WALG_FILE_PREFIX` pointing at a local directory (`backup-push`,
`wal-push`, `backup-fetch`), skipped when the binary is absent.
