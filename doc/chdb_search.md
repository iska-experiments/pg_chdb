chdb_search 0.1.0
=================
## Synopsis

``` psql
# CREATE EXTENSION chdb_search;
CREATE EXTENSION

# CREATE INDEX docs_idx ON docs USING chdb (body, author columnar_ops);
CREATE INDEX

# SELECT id FROM docs WHERE body @@@ 'postgres clickhouse' AND author = 'ann';
 id
----
  7
(1 row)
```

## Description

The chdb_search extension gives a Postgres table a [ClickHouse full-text
index][text index] served by [chDB]. A `chdb` index keeps the indexed columns
in a ClickHouse MergeTree table with a `text` skip index and answers `WHERE`
predicates over tokens, so a query for rows that contain all, any, one or a
phrase of some tokens, match a pattern, or any combination of these, reads
the index, not the heap, and filters the plain columns stored beside the
text there too.

The index is a filter first: a row matches exactly when the tokens the index
derives from it contain the tokens of the query. Order results by columns, by
vector distance with the [chdb_vector] extension, or by
[`chdb.score()`](chdb_search-queries.md#relevance-score), a weighted count of
the query's tokens each row has. A query over an indexed table is planned as a
[custom scan](chdb_search-queries.md#the-custom-scan) that sends the predicates,
the order, the score and the `LIMIT` to ClickHouse as one statement, or as a
scan of the index; a `GROUP BY` or an aggregate over the indexed columns is
[computed by ClickHouse](chdb_search-queries.md#aggregate-pushdown) when the
heap can vouch for it. chDB allows one process per store, so a background worker
per database owns the store and backends talk to it over a Unix socket; libchdb
itself runs in a child of the worker, so a crash in it costs one request. See
[The Worker and the Engine](#the-worker-and-the-engine).

How a search runs, the custom scan, the relevance score, aggregate pushdown,
what a transaction sees and when a server can serve an index, is on the
[queries] page; the store, the worker's protocol, backups and replication
and the debug functions are in the [internals].


## Installation

chdb_search requires PostgreSQL 17 or later and the [chDB] library, libchdb,
v26.9.0 or later. Build and install it with the rest of the distribution (see
the README). It installs the `chdb_search_engine` program beside
`chdb_helper`; the engine links libchdb, so a dynamic build needs the server
to find `libchdb.so` through `ldconfig` or its `LD_LIBRARY_PATH`.

Then create the extension as a superuser in each database that needs it:

```sql
CREATE EXTENSION chdb_search;
```

The extension installs into the `chdb` schema, which every role may use. Put
it on the `search_path`, or write `OPERATOR(chdb.@@@)` and `chdb.text_ops`.
The worker needs no `shared_preload_libraries` entry: the first backend that
needs one starts it. Preloading the library is still advisable, as it is
what lets a `DROP` remove an index's store; see [Dropping](#dropping).

## Creating an Index

```sql
CREATE INDEX name ON table USING chdb (
    column [ opclass [ (option = value [, ...]) ] ] [, ...]
) [ WITH (vacuum_optimize_ratio = fraction) ];
```

One `chdb` index backs one MergeTree table. Each column names an operator
class that decides how it is indexed, with options for that column in
parentheses after the class. Only permanent tables can be indexed, the
column names `ctid` and `xmin` are reserved by the ClickHouse table, and a
text column in `columnar_ops` needs the `"C"` or `"POSIX"` collation, as
ClickHouse compares bytes.

### Operator Classes

| Class            | Type     | Indexed as                                    |
| ---------------- | -------- | --------------------------------------------- |
| `text_ops`       | `text`   | `text` skip index over the tokenized value    |
| `text_array_ops` | `text[]` | `text` index with the `array` tokenizer       |
| `columnar_ops`   | see text | stored and filterable, no text index          |
| `vector_*_ops`   | `vector` | HNSW; provided by the [chdb_vector] extension |

`text_ops` and `text_array_ops` are the defaults for their types; every
other type defaults to `columnar_ops`, which admits the types whose
comparison operators (`=`, `<`, `<=`, `>`, `>=`) are in its family: `int2`,
`int4`, `int8`, `float4`, `float8`, `numeric`, `bool`, `date`, `timestamp`,
`timestamptz`, `uuid`, `text` and the types binary coercible to them, such as
`varchar`; a type with none is refused. The comparisons are pushed down
with a text predicate as one ClickHouse query.

### Per-Column Options

Options are operator class parameters of `text_ops`, so each column has its
own. `text_array_ops` takes none: each element is one token, lowercased.

*   `tokenizer`: `splitByNonAlpha` (the default), `splitByString`,
    `splitByRegexp`, `ngrams`, `sparseGrams`, `icu`, `asciiCJK` or `array`.
*   `tokenizer_arg`: the locale of `icu` or the pattern of `splitByRegexp`,
    which require it, or the separator characters of `splitByString`.
*   `preprocessor`: `lowerUTF8` (the default), `lower`, `caseFoldUTF8`,
    `extractTextFromHTML` or `none`.
*   `raw_preprocessor`: a ClickHouse expression used instead of
    `preprocessor`. Superusers only.
*   `ngram_size`: the n of `ngrams`, from 1 to 8; defaults to 3.
*   `support_phrase_search`: `true` to allow `@@~` and `has_phrase`.

Tokenizers and preprocessors come from a fixed allowlist, so an index
definition cannot run arbitrary ClickHouse expressions; `raw_preprocessor`
is the escape hatch, spliced into the DDL as written, for superusers only.

```sql
CREATE INDEX docs_idx ON docs USING chdb (
    body  text_ops (tokenizer = 'ngrams', ngram_size = 3,
                    support_phrase_search = true),
    title text_ops (tokenizer = 'splitByString', tokenizer_arg = ' ,'),
    price
) WITH (vacuum_optimize_ratio = 0.5);
```

## Functions and Operators

All functions live in the `chdb` schema. Each has a Postgres implementation,
so a sequential scan and the heap recheck return the same rows as the index,
and a ClickHouse translation the index uses.

| Operator         | Function                          | ClickHouse     |
| ---------------- | --------------------------------- | -------------- |
| `col @@@ 'a b'`  | `chdb.has_all_tokens(col, 'a b')` | `hasAllTokens` |
| `col @@? 'a b'`  | `chdb.has_any_tokens(col, 'a b')` | `hasAnyTokens` |
| `col @@= 'a'`    | `chdb.has_token(col, 'a')`        | `hasToken`     |
| `col @@~ 'a b'`  | `chdb.has_phrase(col, 'a b')`     | `hasPhrase`    |
| `col @@/ 're'`   | `chdb.regex(col, 're')`           | `match`        |
| `col @@% 'pat%'` | `chdb.wildcard(col, 'pat%')`      | `LIKE`         |
| `col @@@ query`  | `chdb.query_matches(col, query)`  | the tree       |
| none             | `chdb.score(k [, 'col'])`         | see below      |

`chdb.score()` is the [relevance score](chdb_search-queries.md#relevance-score)
of a row, which only the custom scan computes.

A `chdb.query` combines any of these with `&&`, `||` and `!`, weights a
term with `chdb.boost` and relaxes a phrase with a slop:

```sql
SELECT id FROM docs WHERE body @@@ 'postgres clickhouse';
SELECT id FROM docs
 WHERE body @@@ (chdb.match_all('running shoes') && !chdb.term('boots'));
```

The [query language] page has every operator and builder, what the patterns
and the slop mean, and where the Postgres implementations, which know the
default tokenizer only, part from an index built with another.

## VACUUM

`VACUUM` removes the dead heap tuples from the index with a lightweight
`DELETE`, and runs `OPTIMIZE TABLE ... FINAL` when the dead fraction of the
rows it read exceeds
[`chdb_search.vacuum_optimize_ratio`](#chdb_searchvacuum_optimize_ratio) or
the index's own `vacuum_optimize_ratio` option; until then stale entries
cost space and a heap visit, not correctness. It also drops the tables of
superseded builds and the staging tables of transactions that are over.

## Dropping

`DROP INDEX`, and the `DROP TABLE`, `DROP SCHEMA ... CASCADE` or `DROP
DATABASE` that takes an index with it, unlinks the index relation and the
blobs in its pages with it, as Postgres does for any index. The engine's
table is dropped when the transaction commits, if the dropping session has
the library loaded; put `chdb_search` in `session_preload_libraries` or
`shared_preload_libraries` to make that every session. A table the drop
missed costs nothing but an entry in the engine's cache, which goes when
the database's worker next starts, and a session that loaded the library
on demand inside a `DROP` says so in the log.

The worker is a session of its database: `DROP DATABASE` refuses while it
runs, `DROP DATABASE ... WITH (FORCE)` stops it and proceeds.

## The Worker and the Engine

The worker for a database appears in `pg_stat_activity` with `backend_type`
`chdb_search worker`, connected to that database. It listens on a unix
socket, on Linux the abstract name `@pg_chdb/<hash>/<database oid>` with
the hash of the data directory's path, which `ss -xl` lists and which only
processes of the server's own user may connect to, elsewhere the file
`$PGDATA/pg_chdb/pgsql_tmp/<database oid>.sock`. It keeps the engine's
working directory, a cache rebuilt from the catalog and the index pages
whenever it starts, in `$PGDATA/pg_chdb/pgsql_tmp/<database oid>/`, where
base backups and `pg_rewind` leave it out. It starts when a backend first needs it
and, if it dies, restarts five seconds later or when a backend next asks. At
most 64 databases can have a worker at once.

The worker never loads libchdb. It forks `chdb_search_engine` on the first
request, and that process opens the store and runs every statement. If the
engine dies, the request that finds it dead fails with `chDB engine (pid N)
was terminated by signal 11: Segmentation fault`, the worker logs the death,
and the next request starts a new engine; the backend, the worker and the
rest of the instance are untouched. The worker itself attaches to shared
memory, so a signal death of the worker is a crash of the instance, as of a
backend: the postmaster runs crash recovery, the engine dies with its
parent, and the next call finds a new worker over the same store.

The worker serves one request at a time, so a request can wait behind
another backend's index build or `OPTIMIZE`; a wait inside a commit or abort
callback, where it cannot be cancelled, is bounded by
[`chdb_search.worker_timeout`](#chdb_searchworker_timeout). The [internals]
describe the protocol, the store's layout and the sweeps.

## Settings

### `chdb_search.flush_threshold`

```sql
SET chdb_search.flush_threshold = '256MB';
```

Bytes of insert buffer per index above which a transaction stages its rows in
ClickHouse before a search or commit asks for them (see
[Consistency](chdb_search-queries.md#consistency)). Takes the memory units of
`postgresql.conf`; at least `64kB`. Defaults to `64MB`.

### `chdb_search.vacuum_optimize_ratio`

Fraction of dead index entries above which `VACUUM` runs `OPTIMIZE TABLE
... FINAL`, from `0` to `1`. An index's `vacuum_optimize_ratio` option
overrides it. Defaults to `0.2`.

### `chdb_search.unavailable_index`

What a scan, a commit or a `VACUUM` does with a chdb index whose store is not
available on this server (see
[Availability](chdb_search-queries.md#availability)): `error` raises, so a
broken index is never silent; `skip` lets the planner use another path and
leaves the store alone. Defaults to `error`.

### `chdb_search.worker_timeout`

Seconds a backend waits for a worker to start, and for a busy worker to
answer where the wait cannot be cancelled. From `1` to `3600`; defaults to
`30`.

### `chdb_search.enable_custom_scan`

```sql
SET chdb_search.enable_custom_scan = off;
```

Whether the planner considers [the custom
scan](chdb_search-queries.md#the-custom-scan), and with it the [aggregate
scan](chdb_search-queries.md#aggregate-pushdown). Off, a search is a scan of the
index, which sends the same statement through the access method, row by row.
Defaults to `on`.

### `chdb_search.enable_aggregate_pushdown`

```sql
SET chdb_search.enable_aggregate_pushdown = off;
```

Whether the planner considers the [aggregate
scan](chdb_search-queries.md#aggregate-pushdown), which has ClickHouse compute a
`GROUP BY` and its aggregates. Off, Postgres aggregates the rows of a scan.
Defaults to `on`.

### `chdb_search.custom_scan_cost_factor`

Multiplier on the estimated run cost of a custom scan, which is otherwise
priced as the index scan of the same index is, less the index's own share.
Below `1` the planner prefers the custom scan to the index scan for the same
rows, above `1` the index scan; `0` makes it free. Defaults to `0.5`.

### `chdb_search.mask_oids`

Replaces index OIDs, store generations, transaction ids and WAL positions
by `N` in the ClickHouse statements the index logs at `DEBUG1` and `EXPLAIN`
shows, for tests. Defaults to `off`.

### Resource Limits

`chdb_search.max_memory`, `chdb_search.max_threads` and
`chdb_search.max_parsing_threads` are the same settings as
[`chdb.max_memory`], [`chdb.max_threads`] and [`chdb.max_parsing_threads`]
under the `chdb_search` prefix, with the same units, limits and superuser
requirement; they travel with every request and apply to the engine's query.

## Limitations

*   One process per store: a database's worker is the only reader and
    writer, so reads do not scale with backends, and at most 64 databases
    can have a worker at once.
*   A standby does not serve the index until it is promoted: the pages are
    there, but the worker that reads them for the engine does not run in
    recovery yet.
*   Every byte the engine writes is WAL: a part's files as it flushes them,
    and again as merges rewrite them. WAL_NUMBERS_PLACEHOLDER
*   `pg_upgrade` leaves indexes to be rebuilt with `REINDEX`.
*   The Postgres implementations of the operators tokenize as the default
    pipeline does; other tokenizers are usable through the index only, as
    is a `chdb.in_column` query.
*   The custom scan plans a table of its own: a search inside a join takes
    it when the search predicate names constants or the query's parameters,
    while one that depends on the other side of the join, as `LATERAL` does,
    is served by the index scan.
*   `chdb.score()` is an idf-weighted overlap, not BM25: ClickHouse's text
    index holds no term frequencies. It is computed by the custom scan
    only, in a `SELECT` with a text search and without row locks, and a
    join evaluates it when the scan is directly below the join that
    returns it.
*   The aggregate scan needs every page of the heap all-visible, so a table
    written since its last `VACUUM` is aggregated by Postgres until the next
    one, through the plan the scan carries; a table never vacuumed is never
    aggregated by the store.

## Authors

*   [David E. Wheeler](https://justatheory.com/)
*   [serprex](https://github.com/serprex)

## Copyright

Copyright (c) 2026, ClickHouse

  [chDB]: https://clickhouse.com/chdb
    "chDB - fast, reliable, and scalable in-process database"
  [text index]: https://clickhouse.com/docs/engines/table-engines/mergetree-family/textindexes
    "ClickHouse Docs: Full-text search with text indexes"
  [chdb_vector]: ./chdb_vector.md "chdb_vector Docs"
  [queries]: ./chdb_search-queries.md "chdb_search Queries"
  [query language]: ./chdb_search-query.md "chdb_search Query Language"
  [internals]: ./chdb_search-internals.md "chdb_search Internals"
  [`chdb.max_memory`]: ./chdb.md#chdbmax_memory "chdb Docs: chdb.max_memory"
  [`chdb.max_threads`]: ./chdb.md#chdbmax_threads "chdb Docs: chdb.max_threads"
  [`chdb.max_parsing_threads`]: ./chdb.md#chdbmax_parsing_threads
    "chdb Docs: chdb.max_parsing_threads"
