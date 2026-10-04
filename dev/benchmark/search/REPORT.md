chdb_search against ParadeDB pg_search on Hacker News
=====================================================

One PostgreSQL 18.6 cluster, one machine, the full Hacker News dataset
(28,737,557 items), and the same queries against a `chdb` index
(pg_chdb's chdb_search, branch `search-benchmark` at `c366dc6`, the tip of
`search-page-store-v2` when the run began) and a `paradedb` index (ParadeDB
pg_search 0.26.0, `main` at `63152b5`). Everything here can be rerun with
`bench.sh`; the raw numbers are in `results/`.

## Headline

ParadeDB answers nine of the ten query shapes faster: by 2x for selective
searches that return tens of thousands of rows, by 4x for a common term
whose ids it reads from its own columnar field, and by two orders of
magnitude where it can stop early (BM25 top-10, `LIMIT 10`). chdb_search
builds its index four to six times faster and with less CPU, answers a
plain `count(*)` slightly faster in a freshly vacuumed table, matches
ParadeDB on batched inserts and VACUUMs faster, but takes 2.3x longer per
single-row commit and writes about 80x the WAL for it.

| | chdb_search | ParadeDB |
|---|--:|--:|
| index build (CREATE INDEX, defaults) | **33.0 s** | 208.2 s |
| REINDEX, wall / CPU seconds | **39.1 s / 190 s** | 169.6 s / 476 s |
| index relation after build | 7.82 GiB | **4.57 GiB** |
| index relation once background merges settle | 12.6 GiB (5.1–5.3 GiB live) | **4.6 GiB** |
| common term, 877k / 799k rows, warm median | 504 ms | **115 ms** |
| same, also selecting a column only the heap has | 558 ms | 527 ms |
| rare term, ~32.6k rows | 44.0 ms | **22.3 ms** |
| two-term conjunction / disjunction | 49.2 / 83.2 ms | **25.5 / 35.8 ms** |
| phrase "open source", default index | 1,255 ms | **42.2 ms** |
| phrase, chdb index with `support_phrase_search` | 177 ms | **42.2 ms** |
| `count(*)` of a term | **3.2 ms** | 6.0 ms |
| top-10 by score (IDF overlap vs BM25) | 1,270 ms | **11.1 ms** |
| top-10 by score with a time range in the heap | 1,352 ms | **415 ms** |
| top-10 by score with time in both indexes | 761 ms | **21.6 ms** |
| `LIMIT 10`, no order | 36.3 ms | **0.32 ms** |
| 10,000 single-row commits (no search index: 161/s) | 30 rows/s | **69 rows/s** |
| WAL per single-row commit (no search index: 637 B) | 146,855 B | **1,832 B** |
| 100 commits of 1,000 rows (no search index: 73,964/s) | **29,753 rows/s** | 27,488 rows/s |
| VACUUM after deleting 1% (three rounds) | **31.1 / 11.5 / 8.2 s** | 175.5 / 16.9 / 31.6 s |

Query latencies are warm medians of 20 runs from the first pass (`base`);
bold marks the better figure. The p99, the first runs, a second pass in the
opposite engine order and a pass after the writes are below.
`chdb.score()` on this branch is an IDF-weighted overlap, not BM25: the
ClickHouse text index has no term frequencies. Vector and hybrid search
were skipped, as the dataset has no vectors.

## What Was Compared

The data is loaded once and copied, so each engine has a heap of its own
with the same rows in the same order and only its own index on it, and
one engine's writes, merges and VACUUMs leave the other's table alone:

```sql
CREATE INDEX hn_text_chdb ON hn     USING chdb (text);
CREATE INDEX hn_pdb_text  ON hn_pdb USING paradedb (id, text) WITH (key_field = 'id');
```

Both are each extension's defaults. The chdb store is a MergeTree table of
`ctid`, `xmin` and `text` with a `text` skip index (tokenizer
`splitByNonAlpha`, preprocessor `lowerUTF8`, no token positions), its parts
kept in the index relation's pages. The ParadeDB index has `text`
tokenized by `unicode_words` with positions and field norms, and `id` and
`ctid` as fast fields; it does not store the text. Both definitions, as
the engines report them, are in `results/index_definitions.txt`.
pg_search 0.26.0 warns that `key_field` is deprecated and a no-op; it was
kept as written.

The two tokenizers differ: `splitByNonAlpha` splits `google.com` and
`google's` at the punctuation, `unicode_words` keeps each as one token. So
"google" matches 877,626 rows in chdb_search and 798,740 in ParadeDB
(+9.9%), and every common-term result set is that much larger on the chdb
side; the other queries' counts agree within 0.1–3.5%
(`results/queries_meta.csv`).

## Setup

**Hardware.** AMD Ryzen AI MAX+ 395 (16 cores, 32 threads, up to
5.19 GHz, 64 MiB L3), 125 GiB RAM, Samsung 970 EVO Plus 1 TB NVMe under
LUKS and btrfs with `compress=zstd:3`, Linux 7.1.9-arch1-2. The machine
was shared: other agents compiled libchdb and ran test suites during the
benchmark, and the one-minute load average recorded with every row of the
CSVs ranges from 3.2 to 24.6 on 32 threads. `lscpu` and `free` are in
`results/env.txt`.

**Software.**

*   PostgreSQL 18.6, built with gcc 16.2.1, `-O2`, `--enable-debug`,
    without assertions and without LLVM (so no JIT), in a private prefix
    both extensions were installed into.
*   chdb_search 0.1 (library 0.1.2) from this branch, built with `make
    -j4`. Its engine runs libchdb 26.9.2.1, a build of chdb-core's
    `callback-object-storage` branch (the callback object storage the page
    store needs; sha256 `57cbf69d…9d47`, recorded in `env.txt` from the
    engine's own memory map); the helper used by the COPY hook links the
    vendored libchdb v26.9.0.
*   ParadeDB pg_search 0.26.0, release build with `cargo pgrx install
    --release` (cargo-pgrx 0.19.2, the version the tree pins), pgvector
    0.8.7 (which pg_search requires). Two deviations from ParadeDB's own
    build: the system rustc 1.98.0 rather than the 1.97.1 in
    `rust-toolchain.toml`, as there is no rustup here; and the link step's
    `-lopenblas`, which comes from the `superkmeans` IVF clusterer, was
    satisfied with a shim (`OPENBLAS_LIB_DIR` pointing at a linker script
    `INPUT(-lcblas -lblas)`) over the system's reference CBLAS, as OpenBLAS
    is not installed. BLAS is used only to cluster vectors; nothing here
    calls it.

**Settings.** `shared_buffers = 8GB`, `work_mem = 256MB`, `max_wal_size =
16GB`, `shared_preload_libraries = 'pg_search, chdb_search'`, listening on
127.0.0.1:54700 without a Unix socket; everything else is the default,
including `maintenance_work_mem = 64MB`, `max_parallel_maintenance_workers
= 2`, `max_parallel_workers_per_gather = 2`, `max_worker_processes = 8`,
`synchronous_commit = on`, autovacuum on, and every `chdb_search.*` and
`paradedb.*` setting (`chdb_search.max_threads = 0` lets ClickHouse use all
cores). The full list is in `env.txt`. The cluster ran in a systemd user
unit of its own, so the CPU seconds in the tables are its cgroup's
`usage_usec`: every backend, parallel worker, the chdb worker and its
engine.

**Dataset.** ClickHouse's public `hacknernews.parquet` (7,134,977,202
bytes), downloaded once (635 s) and loaded from the local file with the
chdb COPY hook: 28,737,557 items of every type through 2021-10-03,
24,139,205 of them with text. A few values contain NUL bytes, which
Postgres text cannot hold; `encoding_check 'remove'` drops them. The COPY
took 200.9 s (143k rows/s); with the primary key, the copy to `hn_pdb`,
`VACUUM (FREEZE, ANALYZE)` of both and a checkpoint, `load.sql` took
893.5 s (`results/load.csv`). Each table is about 13 GiB with its TOAST;
`hn`'s heap is 1.1% larger than `hn_pdb`'s (1,676,542 against 1,658,698
pages), as the hook's COPY fills pages slightly less tightly than
`INSERT ... SELECT`.

## Method

Each query in `queries.sql` runs through `pgbench -n -c 1 -j 1 -t 23`: one
connection, the simple protocol, so every run parses, plans, executes and
ships every row to the client, which discards them. The cluster is
restarted before each engine's block of queries. Runs 1–3 are reported as
cold and runs 4–23 as warm; the medians and p99 are of the warm runs, and
with 20 samples the nearest-rank p99 is the slowest of them. "Cold" means
cold Postgres buffers and a newly started chdb engine, not a cold disk: the
OS page cache could not be dropped without root. The row counts and plans
come from one `EXPLAIN (ANALYZE, TIMING OFF)` per query
(`results/plans/`). Before every write and maintenance step the script
waits until the cluster writes less than 1 MB of WAL in 10 s, so that the
previous step's background merges (chdb parts, ParadeDB segments) do not
land in the next measurement. WAL is the `pg_current_wal_lsn()`
difference, cluster-wide.

The three passes over all queries: `base` (chdb_search's block first),
`base_rev` (ParadeDB's first) and `after_writes`. The supplementary passes
`phrase_idx` and `time_in_index` rebuild one or both indexes first.

## Index Builds and Sizes

| engine | step | seconds | CPU s | WAL | index relation | parts / segments | load avg |
|---|---|--:|--:|--:|--:|--:|--:|
| chdb_search | create index | 33.0 | not measured | 7.89 GiB | 7.82 GiB | 38 | 7.10 |
| ParadeDB | create index | 208.2 | not measured | 9.34 GiB | 4.57 GiB | 32 | 24.55 |
| chdb_search | create index (support_phrase_search) | 49.3 | 358.7 | 10.31 GiB | 10.22 GiB | 38 | 9.96 |
| chdb_search | create index (again) | 41.9 | 181.6 | 7.84 GiB | 7.77 GiB | 38 | 9.42 |
| chdb_search | create index (text; time columnar_ops) | 42.6 | 158.4 | 7.83 GiB | 7.76 GiB | 38 | 7.06 |
| ParadeDB | create index (id; text; time) | 196.7 | 504.0 | 4.92 GiB | 4.93 GiB | 32 | 8.66 |
| chdb_search | REINDEX | 39.1 | 190.5 | 7.90 GiB | 7.84 GiB | 38 | 13.77 |
| ParadeDB | REINDEX | 169.6 | 475.8 | 4.63 GiB | 4.62 GiB | 32 | 5.58 |
| ParadeDB | REINDEX, 6 workers, `maintenance_work_mem = 1GB` | 93.6 | 505.3 | 5.15 GiB | 5.13 GiB | 32 | 11.45 |

The CPU column was added after the first two builds; the REINDEX rows
measure the same work. The last row is the only one off the default
settings: ParadeDB's build warns that only two parallel workers were
available and suggests raising `max_parallel_maintenance_workers`, which
then needs 15 MB of `maintenance_work_mem` per worker. ParadeDB's first
build ran while the machine's load rose to 24.6, which may account for
part of its 208 s against 170 s for the REINDEX.

The chdb store keeps the text itself, compressed, beside its text index,
and ParadeDB keeps postings with positions but not the text, so the chdb
index is the larger right after a build. It grows afterwards: background
merges fold the 38 parts of a build into a handful, writing the merged
parts into new pages before the old ones are freed, and the page store
keeps freed pages for reuse rather than giving them back. Once the merges
settle the relation is 12.6 GiB holding 5.1–5.3 GiB of live blobs
(`chdb_search_blobs`), in 6–13 parts. ParadeDB's index stayed at
4.6 GiB; its segments rose only with the writes (32 → 41) and went back to
32 with the REINDEX.

## Queries

The SQL is in `queries.sql`; the shapes, with chdb_search's operator
first: `term_common` `text @@@ 'google'` / `&&&`, `term_rare` the same
with 'postgres', `conjunction` 'google privacy' (`@@@` / `&&&`),
`disjunction` 'postgres mysql' (`@@?` / `|||`), `phrase` 'open source'
(`@@~` / `###`), each `SELECT id`; `count` `count(*)` of 'google';
`top10_score` the disjunction 'google privacy' ordered by `chdb.score(id)`
/ `pdb.score(id)` `DESC LIMIT 10`; `top10_score_time` the same with `time
>= '2020-01-01'`; `limit10` `SELECT id ... 'google' LIMIT 10`;
`term_common_heap` `SELECT id, "by"` for 'google', a column neither index
holds. Every query ran on both sides, and none errored. (The query
language page says `@@~` needs `support_phrase_search`; on this branch the
default index accepts it and checks each candidate's text.)

Milliseconds; "first run" is run 1 of the 23.

### base (chdb_search's block first)

| query | rows chdb | rows ParadeDB | chdb median | chdb p99 | ParadeDB median | ParadeDB p99 | chdb / ParadeDB | first run chdb | first run ParadeDB |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| term_common | 877626 | 798740 | 504 | 530 | 115 | 140 | 4.38x | 14066 | 309 |
| term_rare | 32746 | 32539 | 44.0 | 150 | 22.3 | 26.3 | 1.97x | 546 | 34.7 |
| conjunction | 26498 | 25610 | 49.2 | 69.3 | 25.5 | 28.1 | 1.93x | 109 | 50.0 |
| disjunction | 67327 | 66415 | 83.2 | 91.8 | 35.8 | 38.7 | 2.33x | 728 | 61.5 |
| phrase | 212166 | 211972 | 1255 | 1354 | 42.2 | 47.6 | 29.73x | 5237 | 53.0 |
| count | 1 | 1 | 3.2 | 4.2 | 6.0 | 9.1 | 0.54x | 6.2 | 16.9 |
| top10_score | 10 | 10 | 1270 | 1334 | 11.1 | 13.0 | 114.81x | 1386 | 19.8 |
| top10_score_time | 10 | 10 | 1352 | 1525 | 415 | 481 | 3.25x | 1253 | 5959 |
| limit10 | 10 | 10 | 36.3 | 39.9 | 0.3 | 0.5 | 113.50x | 14.0 | 3.6 |
| term_common_heap | 877626 | 798740 | 558 | 584 | 527 | 606 | 1.06x | 4420 | 2394 |

`term_common_heap` was timed in a separate pass after the others, each
engine again right after a restart, hence its first runs.

### base_rev (ParadeDB's block first)

| query | chdb median | chdb p99 | ParadeDB median | ParadeDB p99 | chdb / ParadeDB | first run chdb | first run ParadeDB |
|---|--:|--:|--:|--:|--:|--:|--:|
| term_common | 511 | 523 | 123 | 134 | 4.16x | 4309 | 381 |
| term_common_heap | 554 | 633 | 571 | 627 | 0.97x | 808 | 2940 |
| term_rare | 44.4 | 47.0 | 22.8 | 24.5 | 1.94x | 285 | 49.9 |
| conjunction | 44.4 | 47.7 | 24.6 | 30.0 | 1.81x | 91.7 | 49.0 |
| disjunction | 73.4 | 81.3 | 31.8 | 40.3 | 2.31x | 326 | 49.8 |
| phrase | 1206 | 1319 | 39.2 | 43.4 | 30.76x | 2545 | 49.1 |
| count | 3.3 | 4.8 | 5.2 | 8.2 | 0.64x | 6.2 | 6.6 |
| top10_score | 1156 | 1323 | 7.9 | 9.2 | 147.04x | 1154 | 18.6 |
| top10_score_time | 1155 | 1196 | 391 | 425 | 2.96x | 1142 | 532 |
| limit10 | 33.9 | 39.4 | 0.3 | 0.5 | 109.26x | 11.4 | 3.1 |

The order of the blocks moves the medians by up to 15%, ParadeDB's top-10
by 29% (11.1 → 7.9 ms); no comparison changes direction.

### after_writes (110,000 more rows per table, before the deletes)

| query | rows chdb | rows ParadeDB | chdb median | chdb p99 | ParadeDB median | ParadeDB p99 | chdb / ParadeDB |
|---|--:|--:|--:|--:|--:|--:|--:|
| term_common | 880655 | 801573 | 499 | 516 | 104 | 109 | 4.79x |
| term_common_heap | 880655 | 801573 | 539 | 551 | 504 | 546 | 1.07x |
| term_rare | 32943 | 32731 | 39.2 | 42.2 | 18.5 | 21.5 | 2.11x |
| conjunction | 26617 | 25725 | 42.3 | 44.3 | 20.3 | 24.1 | 2.09x |
| disjunction | 67631 | 66713 | 70.1 | 73.9 | 30.9 | 38.0 | 2.27x |
| phrase | 213002 | 212808 | 1081 | 1132 | 39.3 | 42.9 | 27.51x |
| count | 1 | 1 | 8.0 | 8.7 | 4.9 | 6.0 | 1.62x |
| top10_score | 10 | 10 | 1098 | 1120 | 8.1 | 8.8 | 135.40x |
| top10_score_time | 10 | 10 | 1097 | 1132 | 403 | 420 | 2.72x |
| limit10 | 10 | 10 | 29.4 | 31.0 | 0.3 | 0.4 | 86.44x |

Autovacuum had visited both tables by then, so both aggregate shortcuts
still applied; the chdb store's `count()` slowed from 3.2 to 8.0 ms, with
11 parts holding rows removed by lightweight `DELETE` (see the caveats)
where the first passes had 38 freshly built ones.

### Supplementary: phrase positions and time in the index

| pass | query | chdb median | chdb p99 | ParadeDB median | ParadeDB p99 | chdb / ParadeDB |
|---|---|--:|--:|--:|--:|--:|
| phrase_idx: chdb `text_ops (support_phrase_search = true)` | phrase | 177 | 196 | 42.2 (base) | 47.6 (base) | 4.2x |
| phrase_idx | term_common | 596 | 705 | 115 (base) | 140 (base) | 5.2x |
| time_in_index: chdb `(text, time)`, ParadeDB `(id, text, time)` | top10_score_time | 761 | 793 | 21.6 | 25.5 | 35.3x |
| time_in_index | top10_score | 995 | 1061 | 8.2 | 9.8 | 121.8x |

`time_in_index` ran last, on tables that had lost 3% of their rows and
been vacuumed three times; `phrase_idx` ran between the query passes and
the writes.

## Writes

10,000 single-row `INSERT`s, each its own transaction, then 100
transactions of 1,000 rows, the same rows into each table (comments copied
from the dataset, new ids), and into an empty copy of the table with only
its primary key as the reference.

| engine | test | rows | seconds | rows/s | CPU s | WAL | WAL/row | parts or segments before → after → 60 s later | load avg |
|---|---|--:|--:|--:|--:|--:|--:|---|--:|
| no search index | single | 10,000 | 61.9 | 161 | 2.4 | 6.1 MiB | 637 B | n/a | 3.55 |
| no search index | batch | 100,000 | 1.4 | 73,964 | 0.3 | 55.3 MiB | 580 B | n/a | 3.23 |
| chdb_search | single | 10,000 | 330.6 | 30 | 342.7 | 1.37 GiB | 146,855 B | 10 → 11 → 11 | 15.46 |
| chdb_search | batch | 100,000 | 3.4 | 29,753 | 3.8 | 328.8 MiB | 3,447 B | 11 → 15 → 11 | 6.69 |
| ParadeDB | single | 10,000 | 144.0 | 69 | 6.3 | 17.5 MiB | 1,832 B | 32 → 37 → 37 | 15.24 |
| ParadeDB | batch | 100,000 | 3.6 | 27,488 | 3.6 | 114.1 MiB | 1,196 B | 37 → 41 → 41 | 6.79 |

pgbench's average latency per commit: 6.2 ms without a search index, 14.4
ms with ParadeDB's, 33.1 ms with chdb_search's (`results/raw/write_*.out`).
The parts are the store's active parts in `system.parts`, read through
`chdb_search_query`; the segments are the rows of
`paradedb.index_info()`.

## REINDEX, DELETE 1% and VACUUM

`DELETE FROM t WHERE id % 100 = m` removes 1% of the original rows spread
over every page, then `VACUUM t`; three rounds, `m` = 0, 1, 2. The first
round follows the REINDEX.

| engine | step | seconds | CPU s | WAL | index relation after | parts / segments after | load avg |
|---|---|--:|--:|--:|--:|--:|--:|
| chdb_search | REINDEX | 39.1 | 190.5 | 7.90 GiB | 7.84 GiB | 38 | 13.77 |
| chdb_search | delete 288,476 rows (m = 0) | 15.9 | 10.0 | 2.18 GiB | 12.60 GiB | 6 | 6.05 |
| chdb_search | VACUUM | 31.1 | 12.6 | 621.5 MiB | 12.61 GiB | 9 | 5.61 |
| chdb_search | delete 288,478 rows (m = 1) | 7.0 | 7.8 | 2.07 GiB | 12.61 GiB | 9 | 8.29 |
| chdb_search | VACUUM | 11.5 | 15.4 | 2.51 GiB | 12.61 GiB | 11 | 6.67 |
| chdb_search | delete 288,476 rows (m = 2) | 5.3 | 7.4 | 192.3 MiB | 12.61 GiB | 11 | 4.74 |
| chdb_search | VACUUM | 8.2 | 13.5 | 298.2 MiB | 12.61 GiB | 13 | 4.44 |
| ParadeDB | REINDEX | 169.6 | 475.8 | 4.63 GiB | 4.62 GiB | 32 | 5.58 |
| ParadeDB | delete 288,476 rows (m = 0) | 10.2 | 9.0 | 2.20 GiB | 4.62 GiB | 32 | 5.26 |
| ParadeDB | VACUUM | 175.5 | 13.3 | 622.2 MiB | 4.62 GiB | 32 | 10.46 |
| ParadeDB | delete 288,478 rows (m = 1) | 9.1 | 8.9 | 2.08 GiB | 4.62 GiB | 32 | 5.83 |
| ParadeDB | VACUUM | 16.9 | 7.1 | 604.9 MiB | 4.63 GiB | 32 | 5.98 |
| ParadeDB | delete 288,476 rows (m = 2) | 5.6 | 7.8 | 268.9 MiB | 4.63 GiB | 32 | 4.12 |
| ParadeDB | VACUUM | 31.6 | 7.1 | 48.4 MiB | 4.63 GiB | 32 | 4.90 |

The DELETEs cost the same on both sides, as they touch only the heap; the
first two rounds' WAL is mostly full-page images after a checkpoint. The
growth of the chdb relation between the REINDEX and the first DELETE is
the merges described under sizes, which the wait before each step let
finish. ParadeDB's first VACUUM spent 175 s for 13 CPU seconds, so it was
mostly waiting, probably on I/O: it ran right after its REINDEX and during
a checkpoint that began at 18:03:48; its later rounds took 17 and 32 s.
chdb_search's VACUUM is a lightweight `DELETE` in the store (the dead
fraction stays below the 0.2 that would trigger `OPTIMIZE ... FINAL`); its
WAL varies with how much of the store ClickHouse rewrites for it.

## Reading the Results

**Where ParadeDB is faster, and why that is plausible.**

*   **Rows come back through two process hops and the heap.**
    ParadeDB's scan runs inside the backend and, for `SELECT id`, reads
    `id` from its columnar fast field without visiting the heap (plan:
    `ColumnarExecState`, `Columnar: id`). chdb_search's engine sends each
    match's `(ctid, xmin)` to the worker, the worker to the backend, and
    the backend fetches every tuple from the heap. When both must read the
    heap, as in `term_common_heap`, they are level (504–571 ms against
    539–558 ms), so most of the 4.4x on `term_common` is the heap fetch
    ParadeDB skips; the steady 2x on the selective searches is that fetch
    plus the per-row transfer, set against a search done in process.
*   **No early stop.** A `LIMIT` without an `ORDER BY` is not pushed into
    the ClickHouse statement (`EXPLAIN` shows no `Pushed Limit`): the
    engine starts the whole search, the scan takes 10 rows (`Store Rows:
    10`) and abandons the rest, about 30 ms. ParadeDB's top-k scan stops
    in the first segment, in 0.3 ms. The round trip itself is small:
    `count(*)`, which moves no rows, takes 3.2 ms through both hops.
*   **Scoring rereads the text.** The text index answers which rows have
    a token but keeps no per-row term data, so `chdb.score()` evaluates
    `hasAllTokens(lowerUTF8("text"), [...])` per needle on every
    candidate, reading and tokenizing about a million texts for 'google
    privacy': 1.0–1.3 s for a top-10. ParadeDB's BM25 reads postings,
    term frequencies and field norms and skips blocks that cannot reach
    the top 10 (block-max WAND): 8–11 ms. The scores also mean different
    things: every chdb top-10 row has both terms and the same score
    (8.629156), ordered by `ctid`, where BM25 ranks by term frequency and
    length (`results/plans/*top10*.out`).
*   **Phrases need positions.** ParadeDB records positions by default.
    The default chdb index does not, so `hasPhrase` checks the text of
    every row with both tokens: 1.2 s. With `support_phrase_search` it
    takes 177 ms against ParadeDB's 42, for a 31% larger index
    (10.2 GiB) and a 49 s build.
*   **Filters left to Postgres hold the LIMIT back.** With `time` in
    neither index, chdb_search sorts every candidate by score in the
    store and Postgres filters them in order (`Store Rows: 122`, 1.1–1.35
    s), while ParadeDB applies the heap filter during its top-k scan
    (391–415 ms). With `time` in both indexes, chdb_search pushes the
    range and the `LIMIT` but still scores every candidate (761 ms);
    ParadeDB answers in 22 ms.
*   **Cold start.** The first query after a restart took chdb_search
    4.3–14 s against ParadeDB's 0.3–0.4 s: the worker forks the engine,
    which attaches the store and reads its parts' metadata as blobs from
    index pages that are not yet in shared buffers.
*   **Mutable segments against parts.** A single-row commit adds the row
    to one of ParadeDB's mutable segments (32 segments became 37 over
    10,000 commits) for about 1.2 kB of index WAL and well under a
    millisecond of CPU. In chdb_search the commit flushes the row to the
    engine as a MergeTree part of its own, which background merges then
    fold into the others, at 34 ms of CPU and 146 kB of WAL per commit
    (the storage docs measure about 29 kB for such a part on their test
    table; the rest here is the merges rewriting it). The parts never
    piled up (10 → 11), but the cost lands on every commit: 30 commits/s
    against 69 for ParadeDB and 161 without a search index, which is this
    disk's fsync rate.

**Where chdb_search is faster.** Building: ClickHouse sorts and writes the
whole table in parallel inside the engine, 33–42 s against 197–208 s with
ParadeDB's default two workers, and with less CPU (190 against 476 CPU
seconds for a REINDEX); with six workers and a gigabyte of
`maintenance_work_mem`, ParadeDB builds in 94 s for 505 CPU seconds.
`count(*)` of a term is the store's own `count()` from the text index:
3.2 ms against 6.0 in a freshly vacuumed table, though 8.0 against 4.9
after the writes. Batched inserts are level (29,753 against 27,488
rows/s), with 2.9x the WAL per row. VACUUM after a 1% delete took 8–31 s
against 17–176 s.

**Sizes and WAL.** Right after a build the chdb index is 1.7x ParadeDB's,
because it holds a compressed copy of the text; after its merges the live
data is close to ParadeDB's (5.1–5.3 against 4.6 GiB), but the relation
keeps its high-water mark of 12.6 GiB. chdb's build writes WAL equal to
its index size (7.9 GiB); ParadeDB's first build wrote 9.3 GiB and its
REINDEX 4.6 GiB for a 4.6 GiB index.

## Caveats and Deviations

*   **A shared machine.** Other agents' builds and tests ran throughout;
    the load average is recorded with every measurement. The two full
    query passes, in opposite engine order, agree within 15% but for
    ParadeDB's top-10 (29%).
*   **Cold is not a cold disk.** The OS page cache was never dropped (no
    root); the first runs measure cold shared buffers and a fresh engine.
*   **The data directory is not under the scratchpad**, as first asked:
    the scratchpad is a tmpfs, RAM shared with the other sessions, with
    about 23 GB free at the start. The cluster lived in
    `~/.cache/bench/pg_chdb-bench/pgdata` (62 GB at the end) and the
    prefix in `~/.cache/bench/pg-bench`.
*   **Two broken write runs came first.** The first attempt failed on a
    VACUUM inside a transaction block; the second created the reference
    table with `LIKE hn INCLUDING ALL`, which copied the chdb index onto
    it, and was stopped after 8,509 single-row commits into `hn`. Those
    rows were deleted and `hn` vacuumed (with `INDEX_CLEANUP on`, as the
    few dead tuples made VACUUM skip the index otherwise) before the
    measured run, which left `hn` with 413 empty trailing pages, its chdb
    store merged from 38 parts to 10 and carrying about 17,000 rows
    removed by lightweight `DELETE`. `bench.sh` now runs the setup
    statements one at a time and builds the reference table without
    indexes.
*   **Autovacuum ran between the write tests.** PostgreSQL 18 scales the
    insert threshold by the unfrozen part of the table, so a thousand
    inserts into the frozen tables trigger it; the last runs on `hn` and
    `hn_pdb` finished in the pauses after chdb_search's and ParadeDB's
    tests, but an earlier run inside a test cannot be ruled out.
*   **The chdb index was rebuilt between the query passes and the
    writes** (the `phrase_idx` pass and its restore), so the writes start
    from a fresh 38-part store that merged to 10 parts before the first
    measured commit.
*   **Interoperability.** pg_search installs `@@@ (anyelement, text)` in
    `pg_catalog`, which is searched first, so `text @@@ 'x'` on the chdb
    table resolves to ParadeDB's operator and fails with "does not contain
    a USING paradedb index" unless the `chdb` schema is on the
    `search_path` (where its exact `(text, text)` match wins) or the
    operator is qualified. The chdb queries run with `search_path = chdb,
    public`.

## Reproducing

```sh
# A cluster with the settings above, both extensions installed, and
#   CREATE DATABASE bench;
#   CREATE EXTENSION chdb; CREATE EXTENSION chdb_search;
#   CREATE EXTENSION pg_search CASCADE;
export PGHOST=127.0.0.1 PGPORT=54700 PGUSER=postgres PGDATA=...
curl -O https://datasets-documentation.s3.eu-west-3.amazonaws.com/hackernews/hacknernews.parquet
SRC=file://$PWD/hacknernews.parquet ./bench.sh load
./bench.sh env
./bench.sh build
./bench.sh queries base chdb pdb
./bench.sh queries base_rev pdb chdb
./bench.sh phrase_index
./bench.sh writes
./bench.sh queries after_writes chdb pdb
./bench.sh maintenance
REINDEX=0 DELETE_MOD=1 ./bench.sh maintenance
REINDEX=0 DELETE_MOD=2 ./bench.sh maintenance
./bench.sh reindex_parallel
./bench.sh time_in_index
./bench.sh summary
```

`./bench.sh all` runs the same sequence. `PG_RESTART` names the command
that restarts the cluster before each engine's queries (here a
`systemd-run` unit of its own, for the CPU accounting); `RESULTS` the
output directory. The run behind this report took place on 2026-10-04
between 16:40 and 18:23 CDT, in the order above.

## Files

*   `load.sql`, `queries.sql`, `bench.sh`: the load, the queries and the
    driver.
*   `results/queries_raw.csv`: every run's latency; `queries.csv` the
    medians and p99s; `queries_meta.csv` the row counts.
*   `results/build.csv`, `writes.csv`, `maintenance.csv`, `load.csv`:
    the other measurements.
*   `results/plans/`: `EXPLAIN ANALYZE` of every query in every pass, and
    the output of the aggregate and top-10 queries.
*   `results/raw/`: pgbench's per-transaction logs and summaries.
*   `results/env.txt`, `index_definitions.txt`: hardware, versions,
    settings, and both indexes as their engines describe them.
