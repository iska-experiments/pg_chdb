# chdb_search: ClickHouse-backed search indexes for PostgreSQL

Design notes for the `chdb_search` and `chdb_vector` extensions. This is the
contract between the sub-projects listed at the end; keep it current when an
interface changes.

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
 backend (chdb_search.so)                 chdb_search worker (per database)
 ┌──────────────────────────┐   unix      ┌────────────────────────────────┐
 │ index AM  (amhandler)    │  socket     │ accept loop, one thread/conn   │
 │ insert buffer → precommit├────────────►│ libchdb session  --path=STORE  │
 │ scan: ctid stream        │◄────────────┤ Native blocks in/out           │
 │ CustomScan / agg pushdown│             │ pg_pages disk callbacks        │
 └──────────────────────────┘             │   (Phase 1) → shared buffers   │
                                          └────────────────────────────────┘
```

### Worker (`src/search/worker.c`, `src/search/client.c`)

* Registered on demand with `RegisterDynamicBackgroundWorker` by the first
  backend in a database that needs it (CREATE INDEX, insert flush, scan).
  `bgw_restart_time = 5s`, `BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION`.
  Flagged in shared memory per database OID so only one starts (a small
  `chdb_search` shmem hash sized by `max_databases` GUC, default 64, attached
  with `shmem_request_hook` when preloaded; without preload a dynamic DSA
  registry via `GetNamedDSMSegment` (PG17+)).
* The worker links libchdb directly (unlike today's forked helper). A libchdb
  crash takes the worker down, Postgres restarts it, backends get an error
  for the in-flight request only. Postmaster is isolated because the worker
  holds no buffer pins across libchdb calls in Phase 0; in Phase 1 the page
  callbacks pin/lock only inside the callback.
* Listens on `$PGDATA/pg_chdb/<dboid>.sock`. Wire protocol reuses the setup
  payload of `src/setup.h` with new commands:
  * `CHDB_CMD_EXEC` run DDL/DML, reply status.
  * `CHDB_CMD_SELECT` stream Native blocks back (reuse `chdb_select_receive`).
  * `CHDB_CMD_INSERT` stream Native blocks in (reuse `chdb_copy_send`).
  Requests carry the index OID and a generation id (see Storage).
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
`amcanreturn = false`, `amgettuple` and `amgetbitmap` both implemented,
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
  INDEX tags_idx tags TYPE text(tokenizer = array),
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
* `amgetbitmap`: same query, fills a TIDBitmap.
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

### Storage and recovery

Phase 0 (local directory): the index relation has one metapage (WAL-logged)
holding a magic, version, a random 64-bit store generation, and the store's
LSN high-water mark written at every flush. The worker checks the
generation and LSN stored in `t`'s `SELECT max(lsn)` against the metapage
on open; mismatch (crash between flush and commit, restore from backup,
`pg_rewind`) marks the index invalid (`indisvalid = false`) and schedules a
rebuild. `DROP INDEX` and `REINDEX` register the store directory for
removal in an `XACT_EVENT_COMMIT` callback; the worker sweeps orphaned
`idx_*` databases whose OID is not in `pg_class` at startup.

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

### Phase 1 host contract (from the chdb-core review)

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

## Parity decisions (2026-10-04)

Kaushik asked for parity with ParadeDB on query language, storage and
same-transaction visibility. Decided:

* **Query language.** First pass: regex and wildcard terms (ClickHouse
  `match` and `LIKE`, both accelerated by the text index), boolean OR/NOT
  trees of chdb predicates pushed as one `WHERE`, and per-clause boosts that
  multiply the IDF score. Phrase slop is exact and evaluated in ClickHouse on
  the candidate set: `hasAllTokens` prefilters, then an expression over
  `tokens(col)` positions checks that the terms fall within the slop. Fuzzy
  and more-like-this wait. Compound queries use builder functions in schema
  `chdb` returning a `chdb.query` value (`chdb.match`, `chdb.phrase(text,
  slop)`, `chdb.regex`, `chdb.wildcard`, `chdb.boost(query, weight)`),
  combined with ordinary SQL `AND`/`OR`/`NOT`; no string parser.
* **Own writes.** `aminsert` still buffers, but the buffer is shipped to the
  transaction's staging table `idx_N.t_tx_<xid>` at the end of each statement;
  scans read `t UNION ALL t_tx_<xid>` for the current transaction; commit
  attaches the staging partition into `t` (`ALTER TABLE ... ATTACH PARTITION
  FROM`) instead of copying; abort drops the staging table. Subtransaction
  rollback deletes that subtransaction's rows from the staging table by the
  subxid column they carry.
* **Storage.** The page store (callback disk from chdb-core PR #256 over
  index-relation pages with generic WAL) replaces the directory store; there
  is one storage mode. Standbys serve searches in this phase: the standby
  worker opens the store read-only from pages with merges stopped. WAL cost
  is measured (WAL bytes per inserted row and per merge, directory store
  versus page store) before part-size and merge defaults are chosen.
