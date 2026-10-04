# chdb_search: ClickHouse-backed search indexes for PostgreSQL

Design notes for the `chdb_search` and `chdb_vector` extensions. This is the
contract between the sub-projects listed at the end; keep it current when an
interface changes. The storage half of the contract (store layout,
recovery, backups and replication) is `chdb_search-storage.md`.

## Goal

`CREATE EXTENSION chdb_search` gives a Postgres table a ClickHouse-backed
index that offers the full feature set of ClickHouse's full-text `text`
index (exact token search, multi-token any/all, phrase search, direct
reads from the index, count from the index) and, with `chdb_vector`,
HNSW vector search over pgvector's `vector` type. Hybrid queries run as one
ClickHouse query. The UX borrows from ParadeDB but names things the way
ClickHouse does. BM25 is out of scope: ClickHouse's index is a filter, not
a ranker.

## Decisions (2026-10-03)

| # | Decision |
|---|----------|
| 1 | One **per-database background worker** owns the chDB store. chDB allows one process per store path (lock file), so backends never link libchdb; they talk to the worker. |
| 2 | **ClickHouse-native naming** with Postgres niceties. Functions mirror ClickHouse (`has_all_tokens`, `has_any_tokens`, `has_token`, `has_phrase`, `tokens`), plus operators for ergonomics. No ParadeDB compatibility layer. |
| 3 | **One multi-column access method** `chdb`. One index per table backs one MergeTree table with text and vector skip indexes. Opclasses choose text vs vector per column. |
| 4 | Vector opclasses live in a separate **`chdb_vector`** extension that requires pgvector. `chdb_search` has no pgvector dependency. |
| 5 | Writes are **buffered per transaction and flushed at pre-commit** as one Native block. **Read-after-commit** consistency only: inside the inserting transaction the index does not see that transaction's own rows, while a sequential scan does, so plan choice decides what a same-transaction query returns. Documented as a limitation; `chdb_search.unavailable_index`-style strictness does not apply here. |
| 6 | **IDF-weighted overlap score**, not BM25. ClickHouse stores no term frequencies, so `chdb.score()` sums the inverse document frequency of the query tokens each row contains; document frequencies come from the text index itself. Deterministic `ORDER BY chdb.score(k) DESC LIMIT n` through the CustomScan. Real BM25 waits on an upstream change (proposal 3). |
| 7 | Scope: index AM **plus CustomScan** (score, top-N, LIMIT pushdown, snippets later) **plus aggregate pushdown** (`count(*)`, `GROUP BY` over indexed columns). |
| 8 | **Crash safety through Postgres pages**: a chDB disk type whose blobs live in index-relation pages written with generic WAL by the worker. Upstream PR to chdb-core. Phase 0 uses a local directory so end-to-end works before that lands. |
| 9 | Tokenizer/preprocessor options are a **curated allowlist**; a `raw_preprocessor` escape hatch is superuser-only. |
| 10 | New extensions target **PostgreSQL 17+** (18 preferred for `extension_control_path`). `chdb` and `chdb_hook` keep 15+. |
| 11 | Commit style: https://github.com/ubicloud/ubicloud/blob/main/COMMIT_MESSAGES.md. Small reviewable commits, draft PRs. |
| 12 | Upstream asks to chDB: pluggable object storage callbacks; multi-process read access to one path. |

## Architecture

```
 backend (chdb_search.so)          chdb_search worker (supervisor)    chdb_search_engine
 ┌──────────────────────────┐ unix ┌─────────────────────────────┐ socket ┌───────────────────┐
 │ index AM  (amhandler)    │socket│ accept loop, relays frames  │  pair  │ libchdb session   │
 │ insert buffer → precommit├─────►│ owns registry slot + socket ├───────►│  --path=STORE     │
 │ scan: ctid stream        │◄─────┤ restarts the engine on death│◄───────┤ Native blocks     │
 │ CustomScan / agg pushdown│      │ pg_pages callbacks (Phase 1)│        │ (crash-isolated)  │
 └──────────────────────────┘      └─────────────────────────────┘        └───────────────────┘
```

### Worker (`src/search/{worker,serve,request,relay,engine_proc}.c`, `src/search/engine/`, `src/search/client.c`)

* Registered on demand with `RegisterDynamicBackgroundWorker` by the first
  backend in a database that needs it (CREATE INDEX, insert flush, scan).
  `bgw_restart_time = 5s`, `BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION`.
  Flagged in shared memory per database OID so only one starts (a small
  `chdb_search` shmem hash sized by `max_databases` GUC, default 64, attached
  with `shmem_request_hook` when preloaded; without preload a dynamic DSA
  registry via `GetNamedDSMSegment` (PG17+)).
* **Supervisor and engine.** The worker never loads libchdb. libchdb aborts or
  segfaults when an allocation fails, and a signal death of a shared-memory
  background worker makes the postmaster run crash recovery for the whole
  cluster, which would defeat the isolation `chdb_helper` gives COPY. So the
  worker is a supervisor: it owns the registry slot, the socket and (Phase 1)
  the page callbacks, and it forks `chdb_search_engine` (installed beside
  `chdb_helper` and set up the same way) on the first request. The engine
  opens the store once, keeps the connection for its life, and serves the
  same framing as the client protocol over a socketpair; it dies with the
  worker (`PR_SET_PDEATHSIG`) so the store lock is released. The worker
  forwards frames in both directions without interpreting Native blocks,
  whole chunks at a time. If the engine dies, `waitpid` and the socketpair's
  EOF tell the worker, which ends any data the client is owed and reports the
  signal in the status frame; the next request respawns the engine. A crash
  therefore costs the request in flight only, the client connection and the
  worker survive, and the postmaster never notices. Settings (`max_memory`,
  `max_threads`, `max_parsing_threads`) travel in every request and the
  engine applies them when they change. Engine stderr is the Postgres log.
  A request arriving after the engine died idle is the one that finds out and
  fails; there is no silent retry, since the supervisor cannot tell a request
  the engine never read from one it half ran.
  `chdb_search_engine_pid()` and `chdb_search_debug_kill_engine(signo)`
  (EXECUTE revoked from PUBLIC) ask the worker for the engine's pid and
  signal it; the test uses them to prove that a SIGSEGV leaves the backend,
  the worker and the postmaster's start time unchanged.
* **Phase 1 consequence.** The page callbacks (read, write, list, remove blob)
  must run in the worker, which has shared buffers; the engine has none. They
  will cross the socketpair in the other direction, as requests from the
  engine to the supervisor interleaved with the response stream of the
  request in flight, which the relay answers rather than forwards (the host
  contract in `chdb_search-storage.md` fixes the rules). Not implemented;
  the framing has no engine-initiated frame yet. Each callback is a round
  trip, so the blob cache of Phase 1 has to sit in the engine process.
* Listens on `$PGDATA/pg_chdb/<dboid>.sock`. Wire protocol reuses the setup
  payload of `src/setup.h` with new commands:
  * `CHDB_CMD_EXEC` run DDL/DML, reply status.
  * `CHDB_CMD_SELECT` stream Native blocks back (reuse `chdb_select_receive`).
  * `CHDB_CMD_INSERT` stream Native blocks in (reuse `chdb_copy_send`).
  Requests carry the index OID and a generation id (see
  `chdb_search-storage.md`): for a non-zero generation the engine checks
  that `idx_<oid>.t_<generation>` exists before running the request and
  otherwise answers `CHDB_STATUS_NO_STORE`, which the client raises with a
  REINDEX hint.
* Store path Phase 0: `$PGDATA/pg_chdb/<dboid>/` holding one chDB database
  `idx_<indexrelid>` per index with table `t`. Phase 1: the same logical
  layout on the `pg_pages` disk whose config names the index relation.

### Access method (`src/search/am.c`, `sql/chdb_search.sql`)

```sql
CREATE INDEX docs_idx ON docs USING chdb (
    body   text_ops (tokenizer = 'splitByNonAlpha', preprocessor = 'lowerUTF8'),
    title  text_ops (tokenizer = 'ngrams', ngram_size = 3),
    tags   text_array_ops,
    author columnar_ops,          -- stored, filterable, aggregatable, no text index
    embedding vector_cosine_ops   -- from chdb_vector
) WITH (store_columns = 'author,created_at');
```

Per-column options use operator class parameters (PG13+ `amoptsprocnum`),
so each column gets its own tokenizer. Allowed tokenizers:
`splitByNonAlpha`, `splitByString`, `splitByRegexp`, `ngrams`,
`sparseGrams`, `icu`, `asciiCJK`, `array`. Allowed preprocessors: `lower`,
`lowerUTF8`, `caseFoldUTF8`, `extractTextFromHTML`, `none`.
`raw_preprocessor = '<expr>'` is accepted only from a superuser.

IndexAmRoutine: `amstrategies = 0`, `amsupport = 1` (options proc),
`amcanmulticol`, `amoptionalkey`, `amcanorderbyop` (vector distance),
`amcanreturn = false`, `amgettuple` only (no `amgetbitmap`, see Scans),
`amcostestimate` via `genericcostestimate` with a low per-tuple cost when a
chdb predicate is present, `ambulkdelete` issues a lightweight
`DELETE FROM t WHERE ctid IN (...)`, `amvacuumcleanup` runs
`OPTIMIZE TABLE t FINAL` when the dead fraction crosses a GUC.

The ClickHouse table:

```sql
CREATE TABLE t (
  ctid UInt64,                     -- (block << 16) | offset
  xmin UInt32,                     -- inserting xid, for recheck diagnostics
  body String, title String, tags Array(String), author String, created_at DateTime64(6),
  embedding Array(Float32),
  INDEX body_idx body TYPE text(tokenizer = splitByNonAlpha, preprocessor = lowerUTF8(body)),
  INDEX title_idx title TYPE text(tokenizer = ngrams(3)),
  INDEX tags_idx tags TYPE text(tokenizer = array, preprocessor = lowerUTF8(tags)),
  INDEX embedding_idx embedding TYPE vector_similarity('hnsw', 'cosineDistance', 1536)
) ENGINE = MergeTree ORDER BY ctid
```

Type mapping reuses `pgch_ch_type_for` from pg-clickhouse-c; `vector` maps
to `Array(Float32)` through `chdb_vector`'s cast to `real[]`.

### Predicates and operators (`sql/chdb_search.sql`, `src/search/ops.c`)

All functions are in schema `chdb`. Each has a plain Postgres
implementation (so sequential scans and the heap recheck give the same
answer) and a ClickHouse translation used by the AM and the CustomScan.

| Postgres | ClickHouse | notes |
|---|---|---|
| `col @@@ 'a b'` or `chdb.has_all_tokens(col, 'a b')` | `hasAllTokens(col, 'a b')` | default operator, strategy 1 |
| `col @@? 'a b'` or `chdb.has_any_tokens(col, 'a b')` | `hasAnyTokens(col, 'a b')` | strategy 2 |
| `chdb.has_token(col, 'a')` | `hasToken(col, 'a')` | strategy 3 |
| `col @@~ 'a b'` or `chdb.has_phrase(col, 'a b')` | `hasPhrase(col, 'a b')` | strategy 4, needs `support_phrase_search` |
| `chdb.tokens(text [, tokenizer, args])` | `tokens(...)` | tokenizer debugging, runs in the worker |
| `col = 'x'`, `col IN (...)`, `col LIKE 'x%'` on `columnar_ops` | same | pushed as filters |
| `embedding <=> q`, `<->`, `<#>` | `cosineDistance`, `L2Distance`, `-dotProduct` | ORDER BY ... LIMIT k only, via `chdb_vector` |

The Postgres fallbacks tokenize with a built-in `splitByNonAlpha` +
lower; for other tokenizers the operator is marked lossy (`xs_recheck`
false, but the planner is told the operator is only usable through the
index: the plain implementation raises when the index is absent, as
pgvector does for unsupported casts). Per-index tokenizers must match the
Postgres-side implementation or the function is index-only; this is
documented.

### Scans

* `amgettuple`: the scan sends one ClickHouse query selecting `ctid`
  (and `_distance` for vector order-bys) with `WHERE` built from the index
  quals and `ORDER BY ... LIMIT` from `orderbys` plus the LIMIT passed down by
  the CustomScan (or `max_limit_for_vector_search_queries`). Rows stream
  back as Native blocks; the AM fills `xs_heaptid` and `xs_orderbyvals`.
  Visibility is rechecked by the executor through the heap fetch, so stale
  entries from aborted transactions are harmless until VACUUM removes them.
* No `amgetbitmap`: a TIDBitmap past `work_mem` makes the bitmap heap scan
  recheck the quals with the Postgres fallbacks, which implement the default
  tokenizer only, so every match of another tokenizer on a lossy page would be
  dropped; the AM streams the whole TID set anyway. EvalPlanQual rechecks
  still evaluate the fallbacks for non-default tokenizers.
* CustomScan (`src/search/customscan.c`): `set_rel_pathlist_hook` adds a
  `chdb_search` path when a relation has a chdb index and the quals include
  our operators. It pushes LIMIT, plain column filters on stored columns, and
  ORDER BY on stored columns or vector distance into one query, returning
  ctids plus requested stored columns (no heap fetch when every output column
  is stored, after visibility check through the visibility map).
* Aggregate pushdown (`create_upper_paths_hook`): `count(*)`, `count(col)`,
  `min/max/sum/avg` and `GROUP BY` over stored columns when every qual is
  pushable. MVCC: the pushed query excludes ctids VACUUM has not yet removed
  only if the heap's visibility map says the pages are all-visible;
  otherwise the path is not generated (same rule as index-only scans).

### Relevance score (`src/search/score.c`, CustomScan only)

`chdb.score(k)` is a placeholder function, like ParadeDB's `pdb.score(key)`:
`k` is any column of the indexed table and only binds the call to that
relation. Outside a chdb CustomScan it raises "chdb.score() needs a chdb
index scan". Inside one, the planner hook replaces it with a column the scan
computes in ClickHouse:

```sql
WITH q AS (SELECT tokens({needle:String}) AS toks)              -- same tokenizer as the index
SELECT ctid,
       arraySum(arrayMap(t -> idf(t) * hasToken(body, t), toks)) AS score
  FROM idx_N.t, q
 WHERE hasAnyTokens(body, {needle:String})
 ORDER BY score DESC
 LIMIT {k:UInt64}
```

`idf(t) = log((N - df(t) + 0.5) / (df(t) + 0.5) + 1)`, the BM25 idf term.
`N` is `count()` of the table and `df(t)` is `count() WHERE hasToken(body, t)`;
both are answered from the text index alone (`ReadFromTextIndexCount`), one
tiny query per distinct token, cached per statement. The score is therefore a
weighted count of matched query terms: a row matching two rare terms
outranks one matching two common ones, and ties are broken by `ctid` for
determinism. With several text columns in the index the per-column scores
are summed; `chdb.score(k, 'body')` restricts to one column.

What this does not do: term frequency, document length normalisation,
phrase proximity. ClickHouse's posting lists hold row ids only, so true BM25
needs the upstream change in `dev/design/chdb-proposals.md` (proposal 3,
term frequencies and document lengths in the text index). The function
signature and the planner plumbing stay the same when that lands.

### Write path (`src/search/insert.c`)

`aminsert` appends `(ctid, values)` to a per-backend, per-index buffer in
`TopTransactionContext`. A `RegisterXactCallback` on `XACT_EVENT_PRE_COMMIT`
ships each buffer as Native blocks over the worker socket with
`CHDB_CMD_INSERT` into `t` and waits for the ack. Abort drops the buffer.
Subtransaction abort drops that subtransaction's rows (buffer entries are
tagged with the current subxid). `ambuild` streams the heap in 8 MiB
blocks through the same path, then `ALTER TABLE t MATERIALIZE INDEX` is
unnecessary because inserts materialize skip indexes; for large builds
`materialize_skip_indexes_on_insert = 0` plus one `MATERIALIZE INDEX` at the
end is faster and is the default above a GUC threshold.

Large transactions: when a buffer exceeds `chdb_search.flush_threshold`
(default 64 MiB) it is flushed early into a staging table `t_tx_<xid>` that
is `INSERT ... SELECT`ed into `t` at pre-commit and dropped on abort.

### Storage

The store layout, the generation and LSN check, the Phase 1 `pg_pages`
disk with its host contract, and the backup and replication guarantees
are in [chdb_search-storage.md](chdb_search-storage.md).

## GUCs

`chdb_search.max_memory`, `max_threads` (worker settings, via `CHDB_GUCS`),
`chdb_search.flush_threshold`, `chdb_search.vacuum_optimize_ratio` (0.2),
`chdb_search.enable_custom_scan`, `enable_aggregate_pushdown`,
`chdb_search.hnsw_candidate_list_size` (256), `chdb_search.vector_rescoring`
(off), `chdb_search.worker_timeout` (30s), `chdb_search.unavailable_index` (`error`).

## Sub-projects and ownership

| id | tree | deliverable | depends on |
|----|------|-------------|------------|
| A | chdb-core `pg-pages-disk` | callback object storage + `chdb_register_object_storage` + example | – |
| B | pg_chdb `search-worker` | bgworker, socket protocol, client API in `src/search/client.h` | – |
| C | pg_chdb `search-am` | AM, SQL script, options, insert buffer, scans, vacuum | B's client.h |
| D | pg_chdb `search-planner` | CustomScan, `chdb.score()`, aggregate pushdown | C's query builder |
| E | pg_chdb `chdb-vector` | `chdb_vector` extension, opclasses, cast | C |
| F | pg_chdb `search-tests` | pg_regress + TAP tests, docs in `doc/chdb_search.md` | C |
| H | pg_chdb `replication-tests` | PITR, standby, logical replication and WAL-G TAP tests; Phase 0 fail-safe in the AM | C, F |

Each lands as a draft PR of small commits. B exposes `client.h` first so C
can compile against it with a stub worker.
