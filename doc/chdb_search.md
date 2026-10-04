chdb_search 0.1.0
=================

## Synopsis

``` psql
# CREATE EXTENSION chdb_search;
CREATE EXTENSION

# CREATE INDEX docs_idx ON docs USING chdb (
    body   text_ops (tokenizer = 'splitByNonAlpha', preprocessor = 'lowerUTF8'),
    author columnar_ops
) WITH (store_columns = 'created_at');
CREATE INDEX

# SELECT id FROM docs WHERE body @@@ 'postgres clickhouse' LIMIT 10;
 id
----
  7
 42
(2 rows)
```

> [!NOTE]
> The index access method, operators, and functions below are specified by
> the [design] and arrive with the access method; sections that depend on it
> are marked **Requires the access method**. Today the extension provides the
> worker and the [debug functions](#debug-functions) only.

## Description

The chdb_search extension gives a Postgres table a [ClickHouse full-text
index][text index] served by [chDB]. A `chdb` index stores the indexed
columns in a ClickHouse MergeTree table with a `text` skip index and answers
`WHERE` predicates over tokens, so a query for rows that contain all, any, or
a phrase of some tokens reads the index rather than scanning the heap.

The index is a filter, not a ranker. There is no BM25 and no relevance score:
a row matches exactly when the tokens the index derives from it contain the
tokens of the query. Order results by columns, or by vector distance with the
`chdb_vector` extension.

chDB allows one process per store, so backends never load libchdb. A
background worker per database owns the store, and backends talk to it over a
Unix socket. See [How It Works](chdb_search-internals.md#how-it-works).

## Installation

chdb_search requires PostgreSQL 17 or later and the [chDB] library, libchdb,
v26.9.0 or later. Build and install it with the rest of the distribution (see
the README). The server, not just the build, must be able to load libchdb, as
the worker `dlopen`s it at startup. Either:

*   Put the directory containing `libchdb.so` on the library path of the
    server process, for example `LD_LIBRARY_PATH=/usr/local/lib` in its
    service environment, or install it where `ldconfig` finds it; or
*   Set [`chdb_search.libchdb_path`](#chdb_searchlibchdb_path) to its full
    path in `postgresql.conf` and reload.

If the library cannot be loaded the worker exits with `FATAL: chdb_search:
could not load "libchdb.so"` and a hint naming both settings.

Then create the extension as a superuser in each database that needs it:

```sql
CREATE EXTENSION chdb_search;
```

The extension needs no `shared_preload_libraries` entry. The first backend
that needs a worker starts it on demand.

## Creating an Index

**Requires the access method.**

```sql
CREATE INDEX name ON table USING chdb (
    column opclass [ (option = value [, ...]) ] [, ...]
) [ WITH (store_columns = 'column [, ...]') ];
```

One `chdb` index per table backs one MergeTree table. Each column names an
operator class that decides how it is indexed, and options for that column
in parentheses after the class.

### Operator Classes

| Operator class   | Column type | Indexed as                                       |
| ---------------- | ----------- | ------------------------------------------------ |
| `text_ops`       | `text`      | ClickHouse `text` index over the tokenized value |
| `text_array_ops` | `text[]`    | `text` index with the `array` tokenizer          |
| `columnar_ops`   | any mapped  | stored, filterable, aggregatable; no text index  |
| `vector_*_ops`   | `vector`    | HNSW; provided by the `chdb_vector` extension    |

`columnar_ops` columns, and those listed in `store_columns`, are kept in the
ClickHouse table so equality, `IN`, and `LIKE 'x%'` filters and aggregates
can be answered without the heap.

### Per-Column Options

Options are operator class parameters, so each column has its own.

| Option             | Applies to  | Values                                   |
| ------------------ | ----------- | ---------------------------------------- |
| `tokenizer`        | `text_ops`  | `splitByNonAlpha` (default), `splitByString`, `splitByRegexp`, `ngrams`, `sparseGrams`, `icu`, `asciiCJK`, `array` |
| `preprocessor`     | `text_ops`  | `none` (default), `lower`, `lowerUTF8`, `caseFoldUTF8`, `extractTextFromHTML` |
| `ngram_size`       | `ngrams`    | integer, the n of the n-grams            |
| `support_phrase_search` | `text_ops` | `true` to allow `has_phrase`, `@@~`  |
| `raw_preprocessor` | `text_ops`  | a ClickHouse expression; superuser only  |

Tokenizers and preprocessors come from a fixed allowlist so that an index
definition cannot run arbitrary ClickHouse expressions. `raw_preprocessor` is
the escape hatch: it is passed to ClickHouse as written and is accepted only
from a superuser. It cannot be combined with `preprocessor`.

```sql
CREATE INDEX docs_idx ON docs USING chdb (
    body      text_ops (tokenizer = 'splitByNonAlpha',
                        preprocessor = 'lowerUTF8',
                        support_phrase_search = true),
    title     text_ops (tokenizer = 'ngrams', ngram_size = 3),
    tags      text_array_ops,
    author    columnar_ops
) WITH (store_columns = 'created_at');
```

## Functions and Operators

**Requires the access method.** All functions live in the `chdb` schema.
Each has a plain Postgres implementation, so a sequential scan and the heap
recheck return the same rows as the index, and a ClickHouse translation the
index uses.

| Operator  | Function                       | ClickHouse        |
| --------- | ------------------------------ | ----------------- |
| `col @@@ 'a b'` | `chdb.has_all_tokens(col, 'a b')` | `hasAllTokens` |
| `col @@? 'a b'` | `chdb.has_any_tokens(col, 'a b')` | `hasAnyTokens` |
| none      | `chdb.has_token(col, 'a')`     | `hasToken`        |
| `col @@~ 'a b'` | `chdb.has_phrase(col, 'a b')` | `hasPhrase`    |
| none      | `chdb.tokens(text [, tokenizer, args])` | `tokens` |

```sql
-- Rows containing both tokens, in any order.
SELECT id FROM docs WHERE body @@@ 'postgres clickhouse';

-- Rows containing at least one.
SELECT id FROM docs WHERE body @@? 'postgres clickhouse';

-- A single token.
SELECT id FROM docs WHERE chdb.has_token(body, 'postgres');

-- The tokens in order; needs support_phrase_search on the column.
SELECT id FROM docs WHERE body @@~ 'full text search';

-- What would the tokenizer make of this? Runs in the worker.
SELECT chdb.tokens('Hello, World!');
```

Matching is exact on tokens: `postgres` does not match `postgresql`. Use an
`ngrams` or `sparseGrams` tokenizer for substring-like matching.

### Debug Functions

These superuser-only functions talk to the worker about a scratch chDB
database named `idx_0`. They exist to test the worker before the access
method does and are not an interface.

*   `chdb_search_version()` returns the library version.
*   `chdb_search_exec(sql)` runs a statement in `idx_0`.
*   `chdb_search_query(sql) AS (...)` runs a query and returns its rows; a
    column definition list is required.
*   `chdb_search_copy_to(regclass, insert_sql)` streams a heap table into an
    `INSERT`, returning the rows sent.
*   `chdb_search_drop()` drops `idx_0`; it is idempotent.

## Consistency

**Requires the access method.**

*   **Flush at commit.** `INSERT`s are buffered per transaction and sent to
    the worker as one block at pre-commit. If the flush fails, the
    transaction fails. An abort drops the buffer, and a rolled back
    subtransaction drops its rows.
*   **Read-after-commit.** Once `COMMIT` returns, every later query sees the
    rows. A transaction does not see its own uncommitted rows through the
    index; use a sequential scan or commit first.
*   **Visibility through the heap.** The index returns tuple ids, and the
    executor rechecks each against the heap, so entries from aborted
    transactions or deleted rows are never returned. Postgres MVCC, not
    ClickHouse, decides what a query sees.

## VACUUM

**Requires the access method.** `VACUUM` removes dead heap tuples from the
index with a lightweight `DELETE`, and runs `OPTIMIZE TABLE ... FINAL` when
the dead fraction exceeds
[`chdb_search.vacuum_optimize_ratio`](#chdb_searchvacuum_optimize_ratio).
Until then stale entries cost space and a heap visit but not correctness.

## The Worker

The worker for a database appears in `pg_stat_activity` with `backend_type`
`chdb_search worker`, connected to that database:

```sql
SELECT pid, datname FROM pg_stat_activity
 WHERE backend_type = 'chdb_search worker';
```

It listens on `$PGDATA/pg_chdb/<database oid>.sock` and keeps its store in
`$PGDATA/pg_chdb/<database oid>/`. It starts when a backend first needs it
and restarts five seconds after dying. See
[The Worker](chdb_search-internals.md#the-worker) for what a crash does.

## Settings

### `chdb_search.libchdb_path`

```ini
chdb_search.libchdb_path = '/usr/local/lib/libchdb.so'
```

The library the worker loads chDB from, as `dlopen` takes it: a bare name is
searched for on the library path. Defaults to `libchdb.so`. Set in
`postgresql.conf`; a reload applies it to the next worker start.

### `chdb_search.worker_timeout`

```sql
SET chdb_search.worker_timeout = 60;
```

Seconds a backend waits for a worker to start. Defaults to `30`.

### `chdb_search.max_memory`

The memory budget, in megabytes, for a worker query, applied as chDB
`max_memory_usage`. Superuser only. Defaults to `0`, leaving it to chDB.

### `chdb_search.max_threads`

The thread budget for a worker query, applied as `max_threads`. Superuser
only. Defaults to `0`, leaving it to chDB.

### `chdb_search.max_parsing_threads`

The thread budget for parallel data parsing in a worker query. Superuser
only. Defaults to `0`.

### Planned Settings

The [design] lists these for the access method; they do not exist yet.

*   `chdb_search.flush_threshold`: transaction buffer size, 64 MiB by
    default, past which rows are flushed early into a staging table.
*   `chdb_search.vacuum_optimize_ratio`: dead fraction that triggers
    `OPTIMIZE`, `0.2` by default.
*   `chdb_search.enable_custom_scan` and
    `chdb_search.enable_aggregate_pushdown`: switch the planner features on
    and off.
*   `chdb_search.hnsw_candidate_list_size` (256) and
    `chdb_search.vector_rescoring` (off): vector search, with `chdb_vector`.

## Limitations

*   One process per store: a store path cannot be opened by two processes,
    so a database's worker is the only reader and writer, and reads do not
    scale with backends.
*   No physical replication in Phase 0: the store is a local directory under
    `$PGDATA/pg_chdb`, not WAL-logged pages, so a standby has no index data.
    Restoring from backup or `pg_rewind` marks indexes invalid until rebuilt.
*   The Postgres-side fallback of the operators tokenizes only with the
    default `splitByNonAlpha` tokenizer. For any other tokenizer the answer
    comes from the index, so the operators need one to be usable.
*   At most 64 databases can have a worker at once.
*   Linux and macOS, as for the rest of the distribution.

See [the internals](chdb_search-internals.md) for the design behind these.

## Authors

*   [David E. Wheeler](https://justatheory.com/)
*   [serprex](https://github.com/serprex)

## Copyright

Copyright (c) 2026, ClickHouse

  [chDB]: https://clickhouse.com/chdb
    "chDB - fast, reliable, and scalable in-process database"
  [text index]: https://clickhouse.com/docs/engines/table-engines/mergetree-family/textindexes
    "ClickHouse Docs: Full-text search with text indexes"
  [design]: ../dev/design/chdb_search.md "chdb_search design notes"
