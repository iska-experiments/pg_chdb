chdb_search Storage
===================

Where a [chdb_search](chdb_search.md) index keeps its store, and what that
means for crash recovery, backups and replication. The authoritative
contract is the [storage design notes][design]; this page describes what
the tree ships, with the source files that do it.

## The Blob Store

The store tables keep their parts on libchdb's callback object storage, so
the engine never writes them itself: each file of a part is a blob the
engine asks the worker for, by a key libchdb chose, over a second
socketpair between the two (`src/search/pagestore/protocol.h`). The
callbacks run on any of the engine's threads, several at once, while the
thread inside libchdb is blocked, so every request carries an id the reply
repeats; the engine writes frames whole under a mutex and a reader thread
of its own matches the replies (`src/search/engine/pagecall.c`,
`pagestore.c`). The worker answers them from its event loop while idle,
since background merges ask with no request in flight, from inside any
wait on the request channel while it relays, and while it waits for a
stopping engine to close its store (`engine_proc.c`, `pagestore/dispatch.c`).

What answers is a table of functions (`pagestore/store.h`: exists,
metadata, read, write begin, append, commit and abort, remove, list, copy,
and the storages held) over the pages of the index relation
(`pagestore/pagebackend.c`, layout in `pages.h`). Block 0 is the metapage;
a directory chain of pages maps each key to its size, commit time and
either the blob's bytes, when they fit beside the key (2 kB), or the first
page of a map chain listing its data pages in order, so a read at an
offset reaches its page in as many hops as there are map pages before it;
a free stack of pages lists the free pages by number. Every write is one
generic WAL record over at most four buffers, so recovery and replication
come from the server. A write takes pages as its data arrives and is
published by its directory entry, written last; a crash before that
leaves pages no entry names, which the worker reclaims when it next opens
a relation whose metapage says a worker took pages in it and did not clear
the flag (`recover.c`). Nothing is flushed at a blob's commit: the commit
record of the transaction behind it follows in the WAL, which is the
durability the engine asks of a host. A copy, which plain_rewritable makes
of each blob before it unlinks a part, shares the source's pages by a
count on the map chain. A crashed engine's pending writes are dropped by
the worker, which logs how many.

The worker reaches the pages by locator, not through the relcache, since a
`CREATE INDEX` or `REINDEX` streams its rows while the relation it writes
is visible to its own backend alone: every request names the relation as
the backend sees it, the worker reads the generation off its metapage and
routes the keys under that generation's prefix to it (`routes.c`). A
generation it has not been told of, a rolled-back rebuild's, is an empty
storage: nothing is found, removing and copying succeed, which is what a
drop of its table needs. Each index has a storage of its own,
`pg_<indexrelid>`, which the engine registers before a table is made on
it, and every storage the worker has routed before it opens the store
again after a crash of its own, since libchdb attaches a persisted table to
its storage by name. `chdb_search_blobs(regclass)` lists an index's blobs
with their size and commit time, read from the pages by the backend.

## The Metapage and the Fail-Safe

The metapage (`pagestore/pages.h`, `meta.c`), WAL-logged through generic
WAL, holds a magic, a version, a random 64-bit generation chosen at every
build, `flushed_lsn`, the WAL position when the store was last written,
the heads of the directory chain and the free stack, and the flag that
tells a worker a predecessor died with pages taken. A rebuild writes its
generation into a new relation, so a rollback leaves the old one whole,
and the next `VACUUM` drops the engine's table of the generation that lost.

Before a scan returns a row, before a commit flushes and before `VACUUM`
deletes, `chdb_search_check_available` asks only whether the pages hold
any blob of the generation the metapage names: the store and the index
being one relation, there is nothing else to compare, and only a build
that never finished leaves none. The worker checks the generation every
request carries as well, and answers `NO_STORE` for a table it does not
have.

## Standbys

The worker starts on a hot standby as on the primary and serves searches
from the replayed pages (`standby.c`). Its engine is started read-only
(`engine/readonly.c`): the callbacks that would write refuse, naming the
reason, merges are stopped for the session, and the worker refuses the
same page requests behind it. Tables are attached with `table_readonly =
1` and without staging tables, from the listing alone, which reads only.
Replay changes the parts under the engine, so a request on a standby
reads a version of the index's blobs, the latest LSN among its metapage
and directory pages, and one that finds it moved detaches the table and
attaches it again (`attach.c`): a search is current to the last record
replayed. When the server is promoted the worker, which waits at most a
second while in recovery, stops the engine, empties its directory and
forgets what it attached and noted; the next request starts a read-write
engine over the same pages, and nothing is rebuilt.

## Staging Tables

A transaction's staging table (see the [internals'
write path](chdb_search-internals.md#the-write-path)) is defined as the
index's table is but for its name, its UUID and its key prefix on the same
storage, `s<generation>_tx_<fxid>`, so that a worker starting afresh
attaches it again from the prefixes the blobs carry (`attach.c`): a
transaction mid-flight survives a worker restart, and a crashed one's table
is still there for `VACUUM` to sweep. Commit attaches its parts to the
index's table with `ALTER TABLE ... ATTACH PARTITION tuple() FROM`, which
copies them from one prefix to the other within the index's storage. A
standby attaches no staging table: they are the primary's transactions',
which no search on the standby reads.

## Backups and Replication

The store is the index relation's pages, so Postgres backs it up and
replicates it as it does any index, and the tests in `t/` prove each case:

*   **Base backups and point-in-time recovery.** `pg_basebackup` copies the
    pages with the heap, and leaves out the engine's directory, a
    `pgsql_tmp`. A restore replays both to the recovery target, so the
    index answers for exactly the rows committed up to it, with no
    `REINDEX`; the worker rebuilds the engine's directory from the catalog.
*   **Streaming replication.** A standby replays the pages with the heap
    and serves searches from them through a read-only engine of its own
    (see [Standbys](#standbys)), current to the last record replayed. A
    promoted standby serves the index at once from the same pages and
    indexes new rows as any primary.
*   **Logical replication.** A subscriber's table keeps its own chdb index
    through ordinary inserts, so the table sync and the apply worker flush
    to the subscriber's store at their commits, and replicated rows are
    searchable there once applied. The publisher's store is not involved.
*   **WAL-G.** `backup-push`, `wal-push`, `backup-fetch` and `wal-fetch`
    back up and restore the index as above, workers running: on Linux a
    worker's socket is a name in the abstract namespace, not a file, so the
    tar WAL-G makes of the data directory meets no socket (tar has none).
    Elsewhere the worker listens on `pg_chdb/pgsql_tmp/<database oid>.sock`,
    which WAL-G leaves out with every `pgsql_tmp`, as `pg_basebackup` does.

## Storage Phases

*   **Phase 0**: a local directory under `$PGDATA/pg_chdb`, written by the
    engine itself, with a `meta` table the access method compared with the
    metapage to refuse a store from another point in time.
*   **Phase 1, stage 1**: chDB's callback object storage, its blobs asked of
    the worker, which kept them as files under the data directory: the
    protocol and the engine's side, with the files a stand-in.
*   **Phase 1, stage 2** (this tree): the blobs in the index relation's
    pages, written with generic WAL. Crash recovery, backups and
    replication come from Postgres, the engine's directory is a cache, a
    standby serves searches through a read-only engine, and the fail-safe
    check asks only whether the pages hold a store.

  [design]: ../dev/design/chdb_search-storage.md "chdb_search storage design notes"
