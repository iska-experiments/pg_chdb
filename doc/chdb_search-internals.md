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

Each database's store is the directory `$PGDATA/pg_chdb/<dboid>/`, a chDB
data path. In it each index owns a chDB database `idx_<indexrelid>` holding
one MergeTree table per build, `t_<generation>`, named after the generation
in the index's metapage, and a `meta` table recording per generation the WAL
position of the last flush into it (`protocol.h`, `ddl.c`).

The first column of a table is `ctid UInt64`, the heap TID packed as
`(block << 16) | offset`, which is the `ORDER BY` key and the join back to
the heap; `xmin UInt32` is the inserting transaction id, a diagnostic only.
The indexed columns follow, named as in Postgres and always quoted, typed by
pg-clickhouse-c's mapping (`text` is `Nullable(String)`, `text[]` is
`Array(Nullable(String))`), each text column with a `text` skip index whose
arguments its operator class options render (`textindex.c`):

```sql
CREATE TABLE idx_16401.t_7342 (ctid UInt64, xmin UInt32,
  "body" Nullable(String), "tags" Array(Nullable(String)),
  "author" Nullable(String),
  INDEX "body_idx" "body" TYPE text(tokenizer = ngrams(3),
    preprocessor = lowerUTF8("body")),
  INDEX "tags_idx" "tags" TYPE text(tokenizer = array,
    preprocessor = lowerUTF8("tags")))
  ENGINE = MergeTree ORDER BY ctid
  SETTINGS fsync_after_insert = 1, fsync_part_directory = 1,
    enable_block_number_column = 1, enable_block_offset_column = 1
```

Parts are fsynced before `COMMIT` returns, and the block number and offset
columns let `VACUUM`'s `DELETE` patch parts as a lightweight update instead
of a mutation. Every statement the access method sends is logged at `DEBUG1`
before it goes, as `chdb_search <what>: <statement>`; with
`chdb_search.mask_oids` the numbers that differ from run to run read `N`.

## The Metapage and the Fail-Safe

The index relation has one page (`meta.c`), WAL-logged through generic WAL:
a magic, a version, a random 64-bit generation chosen at every build, and
`flushed_lsn`, the WAL position when the store was last written. The store's
`meta` table keeps the same record, so the two can be compared: a store and
a relation that agree on the generation and the last flush are from the same
point in time. A rebuild that rolls back leaves the table the metapage still
names untouched, and the next `VACUUM` drops the table of the generation
that lost.

Before a scan returns a row, before a commit flushes and before `VACUUM`
deletes, `chdb_search_check_available` reads the page and asks the store
for its record of the generation, under a heavyweight lock on the page
(shared for checks, exclusive for a flush, which writes the store and then
the page) so that a check never falls between the two writes of another
backend's flush. A server in recovery fails the check too, since the store
is not replicated. The verdict in favour is cached per backend for the
(generation, flushed_lsn) it saw. In `skip` mode a commit that flushes
nothing still advances `flushed_lsn`, so the store can never match again
short of a `REINDEX`. The worker checks the generation every request
carries as well, and answers `NO_STORE` for a table it does not have.

## The Worker

*   **Start.** The first backend in a database that needs a worker claims a
    slot for the database in a registry in dynamic shared memory
    (`registry.c`, 64 slots, states free, starting and running) and
    registers the worker with `RegisterDynamicBackgroundWorker`,
    `bgw_restart_time` five seconds. The worker claims the slot, runs the
    sweep below, listens on `pg_chdb/<dboid>.sock` and logs `worker for
    database N listening`. Backends that find no socket, a stale one or a
    full backlog ask for a worker and retry for `chdb_search.worker_timeout`.
*   **Serving.** One thread, up to 128 connections, one request at a time
    across them (`serve.c`, `request.c`). A request is the setup payload of
    `src/setup.h` with the index OID and the store generation added: the
    command (`SELECT`, `INSERT`, `EXEC` or `DROP`), the resource settings,
    the query; `INSERT` is followed by Native blocks from the client and
    `SELECT` answered by Native blocks to it, as chunks ended by an empty
    one; every request ends with a status frame, `OK`, `ERROR` or
    `NO_STORE`, carrying the error text. The worker forwards frames between
    client and engine without reading the blocks (`relay.c`). Two debug
    commands, answered by the worker itself, return the engine's pid and
    send it a signal.
*   **The engine.** On the first request the worker forks
    `chdb_search_engine` from the package library directory with a
    socketpair end, the store path and its own pid (`engine_proc.c`); the
    engine opens the store once and keeps the session for its life, applies
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
parent, and the next call finds a new worker over the same store. With
`restart_after_crash = off` the instance stops instead. `t/search_worker.pl`
pins both.

## The Write Path

`aminsert` appends `(ctid, xmin, values)` to a per-backend, per-index buffer
in `TopTransactionContext` (`buffer.c`, `rowwriter.c`), never talking to the
worker. At `XACT_EVENT_PRE_COMMIT` each buffer goes to the worker as Native
blocks and the call waits for the status, so a committed row is searchable
when `COMMIT` returns; abort drops the buffers. Each buffer keeps a mark per
subtransaction level that has inserted (`marks.c`): `ROLLBACK TO` rewinds
the writer to the level's mark, `RELEASE` merges it into its parent's. Past
`chdb_search.flush_threshold` a top-level transaction flushes into a staging
table `t_<generation>_tx_<fxid>`, named by the full transaction id, which
pre-commit copies into the table and drops (`staging.c`); inside a savepoint
rows already sent could not be taken back, so the buffer grows until commit,
warning past the threshold and failing past `chdb_search.max_buffer`.
`ambuild` streams the heap in 8 MiB blocks through the same writer
(`build.c`). A `REINDEX` or `TRUNCATE` in the same transaction discards the
rows buffered for the old generation, or sets them aside inside a savepoint
that may yet roll the rebuild back. `PREPARE TRANSACTION` is refused when the
transaction changed a chdb index: a store drop or build cannot be carried
past it.

Drops are deferred to the end of the transaction (`drop.c`): an
`object_access_hook` records every dropped relation that is a chdb index,
and every dropped database, and the commit callback drops the store or
removes the database's directory and socket; an abort drops the table a
rolled-back build made. A `DROP INDEX CONCURRENTLY` drops the store after
the last writer that opened the index before it has committed. Only a
session that has the library loaded runs the hook.

## The Sweeps

Before a worker serves its first request it drops the `idx_<oid>` databases
of its store whose OID is no longer a chdb index, skipping an OID a build
holds locked, and removes the directory and socket of every database no
longer in `pg_database` (`sweep.c`); it logs each. `VACUUM` sweeps inside a
live index's store: the tables of other generations and the staging tables
of transactions that are over (`vacuum.c`).

## Scans

A scan is one ClickHouse `SELECT ctid ... FROM idx_<oid>.t_<generation>
WHERE ...` built from the scan keys (`query.c`, `literal.c`), streamed back
as Native blocks and decoded row by row (`scan.c`). `xs_recheck` is false:
ClickHouse applied the quals, and the heap fetch decides visibility. A
`ctid` past the heap's end, which a store the fail-safe has not refused can
still hold, is skipped. The index returns no columns, yet the planner may
pick an index-only scan when a query needs none, as `count(*)` does, so such
a scan gets an all-null index tuple. There is no `amgetbitmap`: a lossy
bitmap would recheck the quals with the Postgres implementations, which know
the default tokenizer only, and drop every match of another tokenizer.

The custom scan (`planner/`) is the other way a search runs: a
`set_rel_pathlist` hook (`planner/hook.c`) matches the relation's
restriction clauses and the query's pathkeys to the index's operator
families (`planner/match.c`), prices the path below the index scan's
(`planner/cost.c`), and packs what it matched into the plan
(`planner/plan.c`); at execution (`planner/sql.c`, `planner/exec.c`) the
arguments become scan keys and the statement comes from `query.c`, as the
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
(`textindex.c`) and the counts asked of the text index once per statement;
the match in the SELECT list names the tokenizer and applies the
preprocessor itself, as ClickHouse applies the index's only on the index
path. A scoring query gets no other path, since every other one would
evaluate the placeholder, and none in a statement that may recheck rows
under EvalPlanQual, which hands the scan a heap tuple.

## Storage Phases

*   **Phase 0** (this tree): a local directory under `$PGDATA/pg_chdb`. It
    is not WAL-logged and is not copied by physical replication; the
    fail-safe above refuses a store the server cannot prove current.
*   **Phase 1**: a chDB disk whose blobs live in index relation pages
    written with generic WAL by the worker, upstreamed to chDB as a callback
    object storage. Crash recovery and replication then come from Postgres.

## Testing

`test/sql/search_am*.sql` assert the statements the access method
generates, logged at `DEBUG1` with `chdb_search.mask_oids`; they pass with
the worker and with the stub client, `make CHDB_SEARCH_STUB=1`, a
per-backend fake (`client_stub.c`) that accepts every statement and answers
selects from `chdb_search_stub.ctids`, fails on `chdb_search_stub.fail` and
describes its store's generation record in `chdb_search_stub.meta`.
`search_stub_*.sql` run with the stub only; `search_e2e.sql` and
`search_worker.sql` with the worker only. `t/search_*.pl` each pin one
failure mode: a crash with staged rows, a stale or missing store, a
flush racing a check, a worker that never answers, two-phase commit,
`pg_upgrade`, missed drops, multi-round `VACUUM`, concurrent drops, and the
worker and engine under signals.

  [design]: ../dev/design/chdb_search.md "chdb_search design notes"
