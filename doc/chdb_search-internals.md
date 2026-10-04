chdb_search Internals
=====================

How the [chdb_search](chdb_search.md) extension is put together, for
operators who need to know what a failure does and for contributors. The
authoritative contract is the [design notes][design]; this page describes
what the tree ships today, with the source files that do it.

## How It Works

The architecture diagram is in the [design notes' Architecture
section](../dev/design/chdb_search.md#architecture). Three processes take
part. The backend loads `chdb_search.so`: the access method (`am.c`,
`scan.c`, `buffer.c`, `vacuum.c` and the rest of `src/search/`) and the
client of the worker's protocol (`client.c`). The worker is a background
worker per database (`worker.c`, `serve.c`, `request.c`, `relay.c`,
`registry.c`, `sweep.c`); it owns the socket and the store but never loads
libchdb. The engine, `chdb_search_engine` (`src/search/engine/`), is the one
program that links libchdb: the worker forks it, and it opens the store and
runs every statement, so that a crash in libchdb costs one request rather
than the instance.

## The Store

Each index owns a chDB database `idx_<indexrelid>` holding one MergeTree
table per build, `t_<generation>`, named after the generation in the
index's metapage, with a UUID fixed by the two (`protocol.h`, `ddl.c`). The
engine's own metadata about these tables, under
`$PGDATA/pg_chdb/pgsql_tmp/<dboid>/`, is a cache: the worker empties it
when it starts and attaches each index's table from the catalog when a
request first names the index (`attach.c`), with `ATTACH TABLE IF NOT
EXISTS` and the same UUID, so the disk finds the parts where it left them.

The first column of a table is `ctid UInt64`, the heap TID packed as
`(block << 16) | offset`, which is the `ORDER BY` key and the join back to
the heap; `xmin UInt32` is the inserting transaction id, by which a scan
skips the rows of a transaction that never committed. The indexed columns
follow, named as in Postgres and always quoted, typed by pg-clickhouse-c's
mapping (`text` is `Nullable(String)`, `text[]` is
`Array(Nullable(String))`), each text column with a `text` skip index whose
arguments its operator class options render (`textindex.c`):

```sql
CREATE TABLE idx_16401.t_7342 UUID '00004011-0000-0000-0000-1cae00000000'
  (ctid UInt64, xmin UInt32,
  "body" Nullable(String), "tags" Array(Nullable(String)),
  "author" Nullable(String),
  INDEX "body_idx" "body" TYPE text(tokenizer = ngrams(3),
    preprocessor = lowerUTF8("body")),
  INDEX "tags_idx" "tags" TYPE text(tokenizer = array,
    preprocessor = lowerUTF8("tags")))
  ENGINE = MergeTree ORDER BY ctid
  SETTINGS disk = disk(type = 'callback', storage_name = 'pg_16401',
    key_prefix = 'g7342'),
    enable_block_number_column = 1, enable_block_offset_column = 1
```

The parts live on the index's callback object storage, `pg_<indexrelid>`,
whose blobs the worker holds in the index relation's pages (see [The Blob
Store](#the-blob-store)), under a key prefix naming the generation, so that
the blobs of a rebuild being written beside the generation still served
are told apart; a staging table (see [The Write Path](#the-write-path))
has a prefix of its own naming its transaction. The block number and offset
columns let `VACUUM`'s `DELETE` patch parts as a lightweight update instead
of a mutation, which the disk does not allow. Every statement the access
method sends is logged at `DEBUG1` before it goes, as `chdb_search <what>:
<statement>`; with `chdb_search.mask_oids` the numbers that differ from run
to run read `N`.

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
    which `backup-push` fails on with `sockets not supported`; stop the
    database's worker first (`pg_terminate_backend()` on its
    `pg_stat_activity` row), which removes the socket, and it restarts on
    the next request.

## The Worker

*   **Start.** The first backend in a database that needs a worker claims a
    slot for the database in a registry in dynamic shared memory
    (`registry.c`, 64 slots, states free, starting and running) and
    registers the worker with `RegisterDynamicBackgroundWorker`,
    `bgw_restart_time` five seconds. The worker claims the slot, runs the
    sweep below, listens and logs `worker for database N listening`. On
    Linux the socket is the abstract name `@pg_chdb/<hash>/<dboid>`, the
    hash of the data directory's path, device and inode (`serve.c`), so no
    file of it lands in the data directory and two clusters on one host
    never share one; the worker serves only peers of the server's own user,
    which `SO_PEERCRED` names. Elsewhere it is the file
    `pg_chdb/pgsql_tmp/<dboid>.sock`. Backends that find no listener, a stale socket
    file or a full backlog ask for a worker and retry for
    `chdb_search.worker_timeout`.
*   **Serving.** One thread, up to 128 connections, one request at a time
    across them (`serve.c`, `request.c`). A request is the setup payload of
    `src/setup.h` with the index OID, the store generation and the index
    relation's locator added: the command (`SELECT`, `INSERT`, `EXEC` or
    `DROP`), the resource settings,
    the query; `INSERT` is followed by Native blocks from the client and
    `SELECT` answered by Native blocks to it, as chunks ended by an empty
    one; every request ends with a status frame, `OK`, `ERROR` or
    `NO_STORE`, carrying the error text. The worker forwards frames between
    client and engine without reading the blocks (`relay.c`), after noting
    where the index's blobs go and attaching the generation's table if the
    engine lacks it (`attach.c`). Two debug commands, answered by the worker
    itself, return the engine's pid and send it a signal.
*   **The engine.** On the first request the worker forks
    `chdb_search_engine` from the package library directory with two
    socketpair ends, the cache directory and its own pid (`engine_proc.c`);
    the engine opens the store once and keeps the session for its life, applies
    the resource settings of a request when they change, and dies with its
    parent so that the store's lock is released. Its stderr is the server
    log. If the engine dies, the socketpair's end of stream and `waitpid`
    tell the worker, which ends any data the client is owed and reports
    `chDB engine (pid N) was terminated by signal S` in the status, and in
    its log; the next request forks a new engine. There is no retry, as the
    worker cannot tell a request the engine never read from one it half ran.
*   **Stop.** On `SIGTERM` the worker closes its end of the socketpair, which
    the engine reads as the end of its requests, waits up to five seconds
    for it, frees its slot and logs `worker for database N shutting down`.
    `pg_terminate_backend()` and `DROP DATABASE ... WITH (FORCE)` stop it
    that way.

### What a Crash Does

The engine's death is the request in flight's error; the client connection,
the worker and the instance go on:

```
LOG:  chdb_search: chDB engine (pid 2597801) was terminated by signal 11: Segmentation fault
```

The worker attaches to shared memory, so the postmaster cannot tell its
death by a signal from a backend's and treats it as a crash of the instance:

```
LOG:  background worker "chdb_search worker" (PID 2597755) was terminated by signal 9: Killed
LOG:  terminating any other active server processes
LOG:  all server processes terminated; reinitializing
LOG:  database system was not properly shut down; automatic recovery in progress
```

Every session is disconnected, crash recovery runs, the engine dies with its
parent, and the next call finds a new worker over the same pages. With
`restart_after_crash = off` the instance stops instead. `t/search_worker.pl`
pins both, and `t/search_crash.pl` a postmaster killed in the middle of an
insert: recovery leaves the index consistent with the heap, no `REINDEX`.

## The Write Path

`aminsert` appends `(ctid, xmin, values)` to a per-backend, per-index buffer
in `TopTransactionContext` (`buffer.c`, `rowwriter.c`), never talking to the
worker. At `XACT_EVENT_PRE_COMMIT` each buffer goes to the worker as Native
blocks and the call waits for the status, so a committed row is searchable
when `COMMIT` returns; abort drops the buffers. Each buffer keeps a mark per
subtransaction level that has inserted (`marks.c`): `ROLLBACK TO` rewinds
the writer to the level's mark, `RELEASE` merges it into its parent's. Past
`chdb_search.flush_threshold` a transaction flushes into a staging table
`t_<generation>_tx_<fxid>`, named by the full transaction id and created
`AS` the table, whose parts pre-commit attaches to the table with `ALTER
TABLE ... ATTACH PARTITION tuple() FROM` before dropping it (`staging.c`);
rows staged inside a savepoint cannot be rewound, so the rollback of one
excludes its transaction id, which the rows carry as `xmin`, and the table
is then copied with that filter instead of attached.
`ambuild` streams the heap in 8 MiB blocks through the same writer
(`build.c`). A `REINDEX` or `TRUNCATE` in the same transaction discards the
rows buffered for the old generation, or sets them aside inside a savepoint
that may yet roll the rebuild back. `PREPARE TRANSACTION` flushes as commit
does, since `COMMIT PREPARED` and `ROLLBACK PREPARED` run no callback: a
rollback leaves rows the heap hides and `VACUUM` removes. A transaction that
created or dropped a chdb index cannot be prepared, as the store drop or
build cannot be carried past it.

Drops are deferred to the end of the transaction (`drop.c`): an
`object_access_hook` records every dropped relation that is a chdb index,
and every dropped database, and the commit callback drops the store or
removes the database's directory (and socket file, off Linux); an abort
drops the table a rolled-back build made. A `DROP INDEX CONCURRENTLY` drops
the store after the last writer that opened the index before it has
committed. Only a session that has the library loaded runs the hook.

## The Sweeps

Before a worker serves its first request it empties the engine's directory,
a cache of what the last engine saw, and removes the directory and socket of
every database no longer in `pg_database` (`sweep.c`); each index's table
is attached again from the catalog on demand. `VACUUM` sweeps inside a live
index's store: the tables of other generations and the staging tables of
transactions that are over (`vacuum.c`), whose blobs go with them.

## Scans

A scan is one ClickHouse
`SELECT ctid, xmin ... FROM idx_<oid>.t_<generation> WHERE ...` built from the
scan keys (`select.c`, `query.c`, `textsearch.c`, `phrase.c`, `literal.c`; a
`chdb.query` tree is `querytree.c`'s, rendered as one expression), streamed
back as Native blocks and decoded row by row (`stream.c`). When the
transaction has rows buffered or staged for the index, the builder ships the
buffered ones to the staging table first and reads both tables as a
`UNION ALL` of the same `SELECT`, each leg with its own `WHERE`, `ORDER BY`
and `LIMIT` so that the skip and vector indexes serve both, ordered and
limited again outside; the staging leg filters out the transaction ids of
savepoints rolled back since their rows were staged. The builder's FROM
clause, `chdb_search_append_from`, serves every statement over the index's
rows: the custom scan's, the score's counts and the aggregate scan's, the
last two reading the union as a subquery with the `WHERE` in each leg. So a
transaction sees its own rows at once, and the rows it has not searched for
travel as one block at commit, as before. `xs_recheck` is false: ClickHouse
applied the quals, and the heap fetch decides visibility, but for the rows
of a transaction known to have ended without committing, which a crash
between a flush and its commit leaves behind and whose TIDs the heap may
have given out again: those are skipped by the id each row carries
(`xmin.c`), and `VACUUM` deletes them. A `ctid` past the heap's end is
skipped too; both scans share the check (`chdb_search_stream_fetchable`).
The index returns no columns, yet the planner may pick an index-only scan
when a query needs none, as `count(*)` does, so such a scan gets an
all-null index tuple. There is no `amgetbitmap`: a lossy bitmap
would recheck the quals with the Postgres implementations, which know the
default tokenizer only, and drop every match of another tokenizer.

The custom scan (`planner/`) is the other way a search runs: a
`set_rel_pathlist` hook (`planner/hook.c`) matches the relation's
restriction clauses and the query's pathkeys to the index's operator
families (`planner/match.c`), prices the path below the index scan's
(`planner/cost.c`), and packs what it matched into the plan
(`planner/plan.c`); at execution (`planner/sql.c`, `planner/exec.c`) the
arguments become scan keys and the statement comes from `select.c`, as the
index scan's does, and each ctid is fetched through the table access method
under the query's snapshot. `planner/explain.c` shows the pushed clauses,
the LIMIT and the statement.

`chdb.score()` is a placeholder function (`score.c`) that the planner binds
to the scan (`planner/score.c`): the calls become the scan's outputs, named
in a `custom_scan_tlist` behind the heap columns the query needs, so that
the executor's `setrefs.c` points the target list and the quals at a
virtual scan tuple the scan fills from the heap row and the stream. The
store computes the score as a sum over the needles' tokens of
`log((N - df + 0.5) / (df + 0.5) + 1) * hasAllTokens(col, [token])`, with
the needles tokenized through `tokens()` by the column's own tokenizer
(`textindex.c`) and the counts asked of the text index once per statement
(`counts.c`);
the match in the SELECT list names the tokenizer and applies the
preprocessor itself, as ClickHouse applies the index's only on the index
path. A scoring query gets no other path, since every other one would
evaluate the placeholder, and none in a statement that may recheck rows
under EvalPlanQual, which hands the scan a heap tuple.

The aggregate scan (`planner/agg_*.c`) is a `create_upper_paths` hook
(`agg_hook.c`) for a `GROUP BY` or aggregate over one such relation whose
clauses all go to the store: it matches the grouped target's columns and
aggregates to the index (`agg_match.c`), plans a scan of no relation whose
`custom_scan_tlist` is those outputs (`agg_plan.c`), and at execution
(`agg_exec.c`) sends `SELECT <groups>, <aggregates> ... GROUP BY`
(`agg_select.c`) when the heap's visibility map says every page is
all-visible before and after the statement, or else runs the Agg plan it
carries as its child, so the answer is always the snapshot's.

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

## Debug Functions

These superuser-only functions exist to test the worker and the store, and
are not an interface. The first five talk to the worker about a scratch chDB
database named `idx_0`.

*   `chdb_search_version()` returns the library version.
*   `chdb_search_exec(sql)` runs a statement in `idx_0`; `chdb_search_drop()`
    drops `idx_0`, idempotently.
*   `chdb_search_query(sql) AS (...)` runs a query and returns its rows; a
    column definition list is required.
*   `chdb_search_copy_to(regclass, insert_sql)` streams a heap table into an
    `INSERT`, returning the rows sent.
*   `chdb_search_store_table(regclass)` names an index's store table,
    `idx_<oid>.t_<generation>`, for reading it with `chdb_search_query`.
*   `chdb_search_metapage(regclass)` returns the index's magic, version,
    generation and the WAL position of its last flush.
*   `chdb_search_blobs(regclass)` lists the blobs in an index's pages, as
    the worker keeps them for the engine: key, size and commit time.
*   `chdb_search_engine_pid()` returns the pid of the worker's engine, or
    `NULL` before the first request, and
    `chdb_search_debug_kill_engine(signal)` sends it a signal, as a crash
    would.

## Testing

`test/sql/search_am*.sql` and `search_own_writes.sql` assert the
statements the access method generates, logged at `DEBUG1` with
`chdb_search.mask_oids`; they pass with the worker and with the stub
client, `make CHDB_SEARCH_STUB=1`, a per-backend fake (`client_stub.c`)
that accepts every statement, answers selects from
`chdb_search_stub.ctids`, fails on `chdb_search_stub.fail` and finds an
index without a store when `chdb_search_stub.store` is off.
`search_stub_*.sql` run with the stub only; `search_e2e.sql`,
`search_worker.sql`, `search_blobs.sql` and the other end-to-end tests
the Makefile lists with the worker only.
`t/search_*.pl` each pin one failure mode: a backend and a postmaster
crashed with rows in flight, writers racing readers, a worker that never
answers, two-phase commit (a build or drop refused, buffered and staged
rows flushed), `pg_upgrade`, missed drops, multi-round `VACUUM`,
concurrent drops, an engine killed in the middle of a blob, and the worker
and engine under signals; `search_standby.pl`, `search_pitr.pl`,
`search_logical.pl` and `search_walg.pl` prove the storage phase's
guarantees under a streaming standby, point-in-time recovery, a logical
subscription and WAL-G, the last skipping without a `wal-g` on the `PATH`.

  [design]: ../dev/design/chdb_search.md "chdb_search design notes"
