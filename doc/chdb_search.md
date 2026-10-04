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

The index is a filter first: a row matches exactly when the tokens the
index derives from it contain the tokens of the query. Order results by
columns, by vector distance with the [chdb_vector] extension, or by
[`chdb.score()`](#relevance-score), a weighted count of the query's tokens
each row has. A query over an indexed table is planned as a
[custom scan](#the-custom-scan) that sends the predicates, the order, the
score and the `LIMIT` to ClickHouse as one statement, or as a scan of the
index; a `GROUP BY` or an aggregate over the indexed columns is [computed by
ClickHouse](#aggregate-pushdown) when the heap can vouch for it. chDB
allows one process per store, so a background worker per database owns the
store and backends talk to it over a Unix socket; libchdb itself runs in a
child of the worker, so a crash in it costs one request. See [The Worker
and the Engine](#the-worker-and-the-engine).

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

`chdb.score()` is the [relevance score](#relevance-score) of a row, which
only the custom scan computes.

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

## The Custom Scan

A `SELECT` from a table with a chdb index whose `WHERE` holds a search
predicate, or whose `ORDER BY` is a distance operator of an indexed column,
is planned as a `Custom Scan (chdb_search)`: one ClickHouse statement that
applies every predicate the index can take, orders the rows and takes the
`LIMIT`, and returns the tuple ids, which the scan fetches from the heap.
The index scan the planner also considers sends the same statement through
the access method and then pays its per-tuple overhead, so the custom scan
is costed below it;
[`chdb_search.enable_custom_scan`](#chdb_searchenable_custom_scan) turns it
off and
[`chdb_search.custom_scan_cost_factor`](#chdb_searchcustom_scan_cost_factor)
tunes the preference.

```sql
EXPLAIN (COSTS OFF)
SELECT id FROM docs WHERE body @@@ 'running shoes' AND price < 100 AND id > 7;
                                      QUERY PLAN
------------------------------------------------------------------------------
 Custom Scan (chdb_search) on docs
   Filter: (id > 7)
   Pushed Cond: (body @@@ 'running shoes'::text), (price < '100'::numeric)
   ClickHouse: SELECT ctid FROM idx_16401.t_7342 WHERE hasAllTokens("body",
     'running shoes') AND "price" < toDecimal256('100', 0)
```

*   **What goes to ClickHouse.** The operators and the functions of
    [Functions and Operators](#functions-and-operators) on indexed text
    columns, the function forms included, which the index scan cannot use;
    the comparisons on `columnar_ops` columns; and an `ORDER BY` on a
    distance operator of an indexed column, ascending. Everything else, a
    predicate on a column not in the index say, stays with the scan as its
    `Filter`. `EXPLAIN` shows the pushed clauses and the statement, masked
    as the log masks it under
    [`chdb_search.mask_oids`](#chdb_searchmask_oids); `EXPLAIN ANALYZE`
    adds the rows the store returned, which the heap fetch may have thinned,
    and how many statements it took.
*   **The LIMIT.** When the `ORDER BY` went to the store and nothing stayed
    with the scan, and no grouping, `DISTINCT`, window function,
    set-returning function or row lock stands between the scan and the
    `LIMIT`, the `LIMIT` (with its `OFFSET`) goes along, so the store sorts
    and returns that many rows and no more. A row the heap hides, deleted or
    updated since the store took it, is made up for: the scan asks again
    for twice as many, skipping the rows it has seen. A filtered vector
    search is the exception: ClickHouse's HNSW index finds the `LIMIT`
    nearest rows first and applies the `WHERE` to those, so the scan asks
    for as many rows as the index serves, as an index scan does, unless
    `chdb_vector.filter_strategy` is `prefilter` (see [chdb_vector]).
*   **Visibility and rechecks.** The rows come from the heap under the
    query's snapshot, so the custom scan returns what a sequential scan
    would, a transaction's own uncommitted rows included (see
    [Consistency](#consistency)). Row locks and `FOR UPDATE` recheck the
    pushed clauses with their Postgres implementations, as an index scan
    rechecks its conditions.

## Relevance Score

```sql
SELECT id, title, chdb.score(id)
  FROM docs
 WHERE body @@? 'postgres clickhouse'
 ORDER BY chdb.score(id) DESC
 LIMIT 10;
```

`chdb.score(k)` ranks the rows a text search finds. ClickHouse's text index
keeps no term frequencies, so the score is not BM25 but its idf half, an
IDF-weighted overlap: the sum, over the tokens of the query's needles that
the row has, of

    idf(t) = ln((N - df(t) + 0.5) / (df(t) + 0.5) + 1)

where `N` is the rows of the index's store and `df(t)` the rows whose
column has the token, both answered by the text index alone. A row that has
two of the query's tokens outranks a row with one, and a rare token weighs
more than a common one; term frequency, document length and proximity play
no part. `k` is any column of the indexed table, which only binds the call
to that table. With several indexed text columns searched, each column's
score is summed; `chdb.score(k, 'col')` keeps one column's.

The score is a column the [custom scan](#the-custom-scan) has the store
compute, so it is available where the query runs as one: a `SELECT` from
one table with a chdb index, with a text search on an indexed column in its
`WHERE` clause, and without `FOR UPDATE` or `FOR SHARE`. The planner then
takes the custom scan whatever the other paths cost, and the call can
appear anywhere in the query, the target list, the `ORDER BY`, a `WHERE`
on the score, an aggregate or a window function, a join above the scan.
`ORDER BY chdb.score(k) DESC` alone is an order the store serves, ties
broken by physical position so that a `LIMIT` returns the same rows each
time, and takes the `LIMIT` along. Anywhere else, in an `UPDATE`, under a
row lock, over a table without a text search, or with
[`chdb_search.enable_custom_scan`](#chdb_searchenable_custom_scan) off,
the function raises `chdb.score() needs a chdb index scan`.

```sql
EXPLAIN (COSTS OFF)
SELECT id FROM docs WHERE body @@? 'running light'
 ORDER BY chdb.score(id) DESC LIMIT 3;
                                 QUERY PLAN
----------------------------------------------------------------------------
 Limit
   ->  Custom Scan (chdb_search) on docs
         Pushed Cond: (body @@? 'running light'::text)
         Pushed Score: score(id)
         Pushed Limit: 3
         ClickHouse: SELECT ctid, toFloat32(log(3.5 / 3.5 + 1) *
           ifNull(hasAllTokens(lowerUTF8("body"), ['running'],
           'splitByNonAlpha'), 0) + log(4.5 / 2.5 + 1) *
           ifNull(hasAllTokens(lowerUTF8("body"), ['light'],
           'splitByNonAlpha'), 0)) AS _score FROM idx_16401.t_7342
           WHERE hasAnyTokens("body", 'running light')
           ORDER BY _score DESC, ctid LIMIT 3
```

The needles are tokenized by the store with the column's own tokenizer and
preprocessor, so the tokens are the index's whatever the tokenizer; the
counts are asked once per statement, one small query per distinct token,
and `EXPLAIN` asks for them too, as the statement shows the weights. The
counts are of the rows a search reads, the transaction's own included, and
of the versions `VACUUM` has not yet removed. A needle goes through a
`raw_preprocessor` as ClickHouse takes a needle through it, as the value
of the column in a subquery.

## Aggregate Pushdown

A `GROUP BY` or aggregate query over one indexed table whose `WHERE` the
store can apply in full is planned as a `Custom Scan (chdb_search
aggregate)`: one ClickHouse statement that computes the groups and the
aggregates, so that `count(*)` with a text predicate is answered from the
text index without reading a row, and `GROUP BY author` is one pass over
the store. Postgres computes the rest of the query on the result: the
`HAVING` clause, expressions over the aggregates, the `ORDER BY`.

```sql
EXPLAIN (COSTS OFF)
SELECT author, count(*), avg(price) FROM docs WHERE body @@@ 'shoes'
 GROUP BY author HAVING count(*) > 1;
                                                  QUERY PLAN
--------------------------------------------------------------------------------------------------------------
 Custom Scan (chdb_search aggregate)
   Filter: ((count(*)) > 1)
   Pushed Cond: (body @@@ 'shoes'::text)
   ClickHouse: SELECT "author", count(), sumOrNull("price"), count("price") FROM idx_16401.t_7342
     WHERE hasAllTokens("body", 'shoes') GROUP BY "author"
   ->  HashAggregate
         Group Key: author
         ->  Custom Scan (chdb_search) on docs
               Pushed Cond: (body @@@ 'shoes'::text)
               ClickHouse: SELECT ctid FROM idx_16401.t_7342 WHERE hasAllTokens("body", 'shoes')
```

*   **What the store computes.** `count(*)`; `count(col)` of a text or a
    `columnar_ops` column; `min`, `max`, `sum` and `avg` of a `columnar_ops`
    column; and `GROUP BY` text and `columnar_ops` columns of the index, in
    the plain forms: no `DISTINCT`, `ORDER BY` or `FILTER` in the aggregate,
    no grouping sets, no grouping by an expression. Anything else, a clause
    the store cannot apply, `count(DISTINCT x)` or a column outside the
    index say, leaves the whole aggregate to Postgres. The answers are
    Postgres's: `min` and `max` of no rows are `NULL`, a sum has the type
    Postgres gives it, and `avg` is the store's sum and count divided as
    Postgres's `avg` divides them, so it has the same digits. A `float4` or
    `float8` sum is added in double precision by ClickHouse, in an order of
    its own, so its last bits can differ from a sequential scan's.
*   **MVCC.** The store holds a row per heap tuple that a committed
    transaction wrote, the dead ones until `VACUUM`, and knows nothing of
    the query's snapshot, so its count is the heap's only when the heap's
    visibility map says every page is all-visible: no dead tuple is waiting
    for `VACUUM`, and no tuple is from a transaction the snapshot does not
    see. The path is planned only when the map says so, as an index-only
    scan is, and at execution the node asks the map again before the
    statement and after its answer; if the heap changed in between, or the
    store is unavailable in `skip` mode, the node runs the plan Postgres
    would have run instead, which `EXPLAIN` shows as its child and `EXPLAIN
    ANALYZE` marks `Exact Plan` with the reason. So an insert, update or
    delete since the last `VACUUM` costs the shortcut, not the answer. A
    transaction's own rows clear the bits of their pages, so the plan
    Postgres would have run aggregates them; where the map is set
    regardless, after a `COPY FREEZE` into a table the transaction created
    or truncated, the statement reads the transaction's staged rows with
    the table's, as a search does (see [Consistency](#consistency)).
*   **Settings.**
    [`chdb_search.enable_aggregate_pushdown`](#chdb_searchenable_aggregate_pushdown)
    turns it off, as does
    [`chdb_search.enable_custom_scan`](#chdb_searchenable_custom_scan), the
    aggregate scan being a custom scan. The path is priced as a round trip
    and a row per group, so where it applies it wins; a table of a page or
    two is still cheaper to scan.

## Consistency

*   **A transaction sees its own rows.** A search through the index inside
    a transaction finds the rows that transaction has inserted, updated or
    copied so far, as a sequential scan would: the search first ships the
    rows buffered since the last one to a staging table of the
    transaction's own, `idx_<oid>.t_<generation>_tx_<xid>`, then reads it
    with the index's table in one query. Only the transaction reads its
    staging table; other sessions see the rows once it commits. Rows an
    `UPDATE` or `DELETE` replaced or removed are hidden by the heap fetch,
    as every stale index entry is.
*   **Flush at commit.** Rows not yet staged are sent to the worker at
    pre-commit, the staged ones are attached to the index's table in place,
    and `COMMIT` returns once the worker has them all, failing if that
    fails. An abort drops the buffer and the staging table.
*   **Read after commit.** Once `COMMIT` returns, every later query in
    every session sees the rows.
*   **Savepoints.** `ROLLBACK TO` takes the savepoint's rows out of the
    buffer, and hides the ones already staged at once: they stay out of
    every later search and out of the commit.
*   **Visibility through the heap.** The index returns tuple ids and the
    executor fetches each from the heap, so the rows of aborted transactions
    and the old versions that linger in the store until `VACUUM` are never
    returned. Postgres MVCC decides what a query sees.
*   **Large transactions.** Past
    [`chdb_search.flush_threshold`](#chdb_searchflush_threshold) a
    transaction stages its rows as they come, searches or not.
*   **Two-phase commit.** `PREPARE TRANSACTION` flushes as `COMMIT` does, so
    the rows of a prepared transaction are in the store before its fate is
    decided: `ROLLBACK PREPARED` leaves them dead in the heap, hidden until
    `VACUUM` removes them from the store, and `COMMIT PREPARED` makes them
    searchable. A transaction that created or dropped a chdb index cannot be
    prepared, as its store build or drop cannot be carried past `PREPARE`.

## VACUUM

`VACUUM` removes the dead heap tuples from the index with a lightweight
`DELETE`, and runs `OPTIMIZE TABLE ... FINAL` when the dead fraction of the
rows it read exceeds
[`chdb_search.vacuum_optimize_ratio`](#chdb_searchvacuum_optimize_ratio) or
the index's own `vacuum_optimize_ratio` option; until then stale entries
cost space and a heap visit, not correctness. It also drops the tables of
superseded builds and the staging tables of transactions that are over.

## Availability

The store is derived data under the data directory, in this phase neither
WAL-logged nor replicated. Before a scan, a commit's flush or `VACUUM`'s
deletes, the index proves that this server can serve its store: the server
is not in recovery, and the store's record of its last flush agrees with the
index's. A standby, a restore from a backup, a `pg_rewind`, a copied or a
missing store fail that proof, and
[`chdb_search.unavailable_index`](#chdb_searchunavailable_index) decides:
by default the statement fails with `chdb index "name" is not available on
this server` and names the `REINDEX` that rebuilds the store; in `skip` mode
the planner takes another path, a commit keeps its rows from the store, and
the index moves on so that the store can never match it again.

Every build writes a new **generation**: a random id in the index's one
WAL-logged page that names the store table, `idx_<oid>.t_<generation>`. A
`REINDEX`, `TRUNCATE` or table rewrite builds the new generation beside the
old, so a rollback leaves a valid index, and `VACUUM` sweeps the loser. A
request for a generation the store no longer has fails with `chdb index
"name" does not match its store`; `REINDEX INDEX` rebuilds it.

## Backups and Replication

The store is a directory, so Postgres backs it up and replicates it as
files, not as pages, and the [fail-safe](#availability) decides what a copy
is worth. The tests in `t/` prove each case:

*   **Base backups and point-in-time recovery.** `pg_basebackup` copies the
    store with the rest of the data directory, warning that it skips the
    worker's socket. A restore brings the heap to its recovery target and
    the store back as the backup took it, so an index flushed between the
    two is refused until `REINDEX` rebuilds it from the restored heap.
*   **Streaming replication.** A standby has the copy its base backup took
    and starts no worker: a search through a chdb index fails with `Its
    store is not current on a server in recovery`, or in `skip` mode is
    planned without the index. Promotion makes the server ask the store,
    which is as far behind as a restore's; `REINDEX` rebuilds it, and the
    promoted server indexes new rows as any primary.
*   **Logical replication.** A subscriber's table keeps its own chdb index
    through ordinary inserts, so the table sync and the apply worker flush
    to the subscriber's store at their commits, and replicated rows are
    searchable there once applied. The publisher's store is not involved.
*   **WAL-G.** `backup-push`, `wal-push`, `backup-fetch` and `wal-fetch`
    back up and restore the store as above, with one catch: WAL-G tars every
    file under the data directory, and tar has no entry for a socket, so
    `backup-push` fails with `sockets not supported` while a worker listens
    on `pg_chdb/<database oid>.sock`. Stop the database's worker first
    (`pg_terminate_backend()` on its `pg_stat_activity` row), which removes
    the socket; it restarts on the next request.

## Dropping

`DROP INDEX`, and the `DROP TABLE`, `DROP SCHEMA ... CASCADE` or `DROP
DATABASE` that takes an index with it, removes the index's store when the
transaction commits, if the dropping session has the library loaded; put
`chdb_search` in `session_preload_libraries` or `shared_preload_libraries`
to make that every session. A store the drop missed is swept when the
database's worker next starts, and a session that loaded the library on
demand inside a `DROP` says so in the log.

The worker is a session of its database: `DROP DATABASE` refuses while it
runs, `DROP DATABASE ... WITH (FORCE)` stops it and proceeds.

## The Worker and the Engine

The worker for a database appears in `pg_stat_activity` with `backend_type`
`chdb_search worker`, connected to that database. It listens on
`$PGDATA/pg_chdb/<database oid>.sock` and keeps its store in
`$PGDATA/pg_chdb/<database oid>/`. It starts when a backend first needs it
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

Bytes of insert buffer per index above which a transaction stages its rows
in ClickHouse before a search or commit asks for them (see
[Consistency](#consistency)). Takes the memory units of `postgresql.conf`;
at least `64kB`. Defaults to `64MB`.

### `chdb_search.vacuum_optimize_ratio`

Fraction of dead index entries above which `VACUUM` runs `OPTIMIZE TABLE
... FINAL`, from `0` to `1`. An index's `vacuum_optimize_ratio` option
overrides it. Defaults to `0.2`.

### `chdb_search.unavailable_index`

What a scan, a commit or a `VACUUM` does with a chdb index whose store is
not available on this server (see [Availability](#availability)): `error`
raises, so a broken index is never silent; `skip` lets the planner use
another path and leaves the store alone. Defaults to `error`.

### `chdb_search.worker_timeout`

Seconds a backend waits for a worker to start, and for a busy worker to
answer where the wait cannot be cancelled. From `1` to `3600`; defaults to
`30`.

### `chdb_search.enable_custom_scan`

```sql
SET chdb_search.enable_custom_scan = off;
```

Whether the planner considers [the custom scan](#the-custom-scan), and with
it the [aggregate scan](#aggregate-pushdown). Off, a search is a scan of the
index, which sends the same statement through the access method, row by
row. Defaults to `on`.

### `chdb_search.enable_aggregate_pushdown`

```sql
SET chdb_search.enable_aggregate_pushdown = off;
```

Whether the planner considers the [aggregate scan](#aggregate-pushdown),
which has ClickHouse compute a `GROUP BY` and its aggregates. Off, Postgres
aggregates the rows of a scan. Defaults to `on`.

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
*   `chdb_search_engine_pid()` returns the pid of the worker's engine, or
    `NULL` before the first request, and
    `chdb_search_debug_kill_engine(signal)` sends it a signal, as a crash
    would.

## Limitations

*   One process per store: a database's worker is the only reader and
    writer, so reads do not scale with backends, and at most 64 databases
    can have a worker at once.
*   The store is not replicated in this phase: it is a directory under
    `$PGDATA/pg_chdb`, not WAL-logged pages, so a standby cannot serve a
    chdb index, and a restore, a promotion or a `pg_rewind` leaves indexes
    to rebuild with `REINDEX`; WAL-G backs it up only while the worker is
    stopped. See [Backups and Replication](#backups-and-replication). The
    next phase keeps the store in index pages.
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
  [query language]: ./chdb_search-query.md "chdb_search Query Language"
  [internals]: ./chdb_search-internals.md "chdb_search Internals"
  [`chdb.max_memory`]: ./chdb.md#chdbmax_memory "chdb Docs: chdb.max_memory"
  [`chdb.max_threads`]: ./chdb.md#chdbmax_threads "chdb Docs: chdb.max_threads"
  [`chdb.max_parsing_threads`]: ./chdb.md#chdbmax_parsing_threads
    "chdb Docs: chdb.max_parsing_threads"
