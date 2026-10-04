chdb_search Queries
===================

How a search over a [chdb_search](chdb_search.md) index runs: the custom
scan that sends it to ClickHouse as one statement, the relevance score it
computes, the aggregates ClickHouse answers, what a transaction sees, and
when a server can serve an index at all. The operators and the `chdb.query`
builders are on the [query language] page, the settings in the
[reference](chdb_search.md#settings), the store on the [storage] page and
the worker in the [internals].

## The Custom Scan

A `SELECT` from a table with a chdb index whose `WHERE` holds a search
predicate, or whose `ORDER BY` is a distance operator of an indexed column, is
planned as a `Custom Scan (chdb_search)`: one ClickHouse statement that applies
every predicate the index can take, orders the rows and takes the `LIMIT`, and
returns the tuple ids, which the scan fetches from the heap. The index scan the
planner also considers sends the same statement through the access method and
then pays its per-tuple overhead, so the custom scan is costed below it;
[`chdb_search.enable_custom_scan`](chdb_search.md#chdb_searchenable_custom_scan)
turns it off and
[`chdb_search.custom_scan_cost_factor`](chdb_search.md#chdb_searchcustom_scan_cost_factor)
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

*   **What goes to ClickHouse.** The operators and the functions of [Functions
    and Operators](chdb_search.md#functions-and-operators) on indexed text
    columns, the function forms included, which the index scan cannot use; the
    comparisons on `columnar_ops` columns; and an `ORDER BY` on a distance
    operator of an indexed column, ascending. Everything else, a predicate on a
    column not in the index say, stays with the scan as its `Filter`. `EXPLAIN`
    shows the pushed clauses and the statement, masked as the log masks it under
    [`chdb_search.mask_oids`](chdb_search.md#chdb_searchmask_oids); `EXPLAIN
    ANALYZE` adds the rows the store returned, which the heap fetch may have
    thinned, and how many statements it took.
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

A [`chdb.query`](chdb_search-query.md) is scored by the tokens of its
match, term and phrase leaves, each idf multiplied by the `chdb.boost`
weights above the leaf; a token in two leaves counts once, with the larger
weight. A pattern, `@@/` or `@@%` or a leaf of a query, has no tokens to
weigh, nor does a leaf under a `!`, which matches no row the score would
count it for.

```sql
SELECT id, chdb.score(id)
  FROM docs
 WHERE body @@@ (chdb.term('light') || chdb.boost(chdb.term('running'), 2))
 ORDER BY chdb.score(id) DESC;
```

The score is a column the [custom scan](#the-custom-scan) has the store compute,
so it is available where the query runs as one: a `SELECT` from one table with a
chdb index, with a text search on an indexed column in its `WHERE` clause, and
without `FOR UPDATE` or `FOR SHARE`. The planner then takes the custom scan
whatever the other paths cost, and the call can appear anywhere in the query,
the target list, the `ORDER BY`, a `WHERE` on the score, an aggregate or a
window function, a join above the scan. `ORDER BY chdb.score(k) DESC` alone is
an order the store serves, ties broken by physical position so that a `LIMIT`
returns the same rows each time, and takes the `LIMIT` along. Anywhere else, in
an `UPDATE`, under a row lock, over a table without a text search, or with
[`chdb_search.enable_custom_scan`](chdb_search.md#chdb_searchenable_custom_scan)
off, the function raises `chdb.score() needs a chdb index scan`.

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
-----------------------------------------------------------------------------
 Custom Scan (chdb_search aggregate)
   Filter: ((count(*)) > 1)
   Pushed Cond: (body @@@ 'shoes'::text)
   ClickHouse: SELECT "author", count(), sumOrNull("price"), count("price")
     FROM idx_16401.t_7342 WHERE hasAllTokens("body", 'shoes')
     GROUP BY "author"
   ->  HashAggregate
         Group Key: author
         ->  Custom Scan (chdb_search) on docs
               Pushed Cond: (body @@@ 'shoes'::text)
               ClickHouse: SELECT ctid FROM idx_16401.t_7342
                 WHERE hasAllTokens("body", 'shoes')
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
    [`chdb_search.enable_aggregate_pushdown`](chdb_search.md#chdb_searchenable_aggregate_pushdown)
    turns it off, as does
    [`chdb_search.enable_custom_scan`](chdb_search.md#chdb_searchenable_custom_scan),
    the aggregate scan being a custom scan. The path is priced as a round trip
    and a row per group, so where it applies it wins; a table of a page or two
    is still cheaper to scan.

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
    [`chdb_search.flush_threshold`](chdb_search.md#chdb_searchflush_threshold) a
    transaction stages its rows as they come, searches or not.
*   **Two-phase commit.** `PREPARE TRANSACTION` flushes as `COMMIT` does, so
    the rows of a prepared transaction are in the store before its fate is
    decided: `ROLLBACK PREPARED` leaves them dead in the heap, hidden until
    `VACUUM` removes them from the store, and `COMMIT PREPARED` makes them
    searchable. A transaction that created or dropped a chdb index cannot be
    prepared, as its store build or drop cannot be carried past `PREPARE`.

## Availability

The store lives in the pages of the index relation, written through
Postgres's generic WAL like any index: crash recovery, base backups,
point-in-time recovery, streaming replication and `pg_rewind` carry it with
the heap, and no `REINDEX` is needed after any of them. A hot standby
serves searches through the index from its replayed pages, with a worker
and an engine of its own that write nothing; a search there is current to
the last record replayed, and a promoted standby keeps serving, taking
rows from then on. What remains to check before a scan, a commit's flush
or `VACUUM`'s deletes is that the index has a store at all, which only a
build that never finished lacks, and
[`chdb_search.unavailable_index`](chdb_search.md#chdb_searchunavailable_index)
decides what to do with one that has none: by default the statement fails
with `chdb index "name" has no store`; in `skip` mode the planner takes
another path and answers from the heap. The [storage] page has the
details.

Every build writes a new **generation**: a random id in the index's
metapage that names the store table, `idx_<oid>.t_<generation>`. A
`REINDEX`, `TRUNCATE` or table rewrite builds the new generation in a new
relation beside the old, so a rollback leaves a valid index, and `VACUUM`
sweeps the loser's table out of the engine. A request for a generation the
engine no longer has fails with `chdb index "name" does not match its
store`; `REINDEX INDEX` rebuilds it.

Rows the store holds for a transaction that never committed, after a crash
between its flush and its commit, carry the transaction id that wrote them,
and a scan skips them as `VACUUM` deletes them; the heap may have given
their TIDs to other rows since.

  [chdb_vector]: ./chdb_vector.md "chdb_vector Docs"
  [query language]: ./chdb_search-query.md "chdb_search Query Language"
  [internals]: ./chdb_search-internals.md "chdb_search Internals"
  [storage]: ./chdb_search-storage.md "chdb_search Storage"
