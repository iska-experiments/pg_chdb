chdb_vector 0.1.0
=================

## Synopsis

``` psql
# CREATE EXTENSION vector;
# CREATE EXTENSION chdb_search;
# CREATE EXTENSION chdb_vector;

# CREATE INDEX ON docs USING chdb (body, embedding vector_cosine_ops);

# SELECT id FROM docs ORDER BY embedding <=> '[0.1, 0.2, 0.3]' LIMIT 10;

# SELECT id FROM docs
   WHERE body @@@ 'running shoes'
   ORDER BY embedding <=> '[0.1, 0.2, 0.3]' LIMIT 10;
```

## Description

chdb_vector adds [pgvector] `vector` columns to the `chdb` index access
method of [chdb_search], so that one index answers a text predicate and a
nearest-neighbour order in one ClickHouse query. It requires both
extensions, and chdb_search does not require it: the vector operator
classes live here so that chdb_search has no pgvector dependency.

A `vector(n)` column becomes an `Array(Float32)` column in ClickHouse
with a `vector_similarity('hnsw', <function>, n)` skip index. The column must
have a dimension: `vector` without a typmod cannot be indexed. Values are
encoded through pgvector's `vector::real[]` cast; a NULL vector cannot be
stored, as ClickHouse wants every array at the index's dimension.

An index scan ordered by a distance operator sends one query in the shape
ClickHouse's HNSW index serves, `ORDER BY <function>(col, q) LIMIT n`, with
`n` the server's `max_limit_for_vector_search_queries` (1000), the most the
index returns: as a pgvector scan returns at most `hnsw.ef_search` rows, a
`LIMIT` above that gets those rows only. The settings below go with it.
The [custom scan] of chdb_search sends the query's own `LIMIT` instead when
nothing filters the search, or when `chdb_vector.filter_strategy` is
`prefilter`: with `auto` or `postfilter` ClickHouse applies the `WHERE` to
the `LIMIT` nearest candidates, which would return fewer rows than match.

## Operator Classes

| Class               | Operator | ClickHouse       | Order  |
| ------------------- | -------- | ---------------- | ------ |
| `vector_l2_ops`     | `<->`    | `L2Distance`     | `ASC`  |
| `vector_cosine_ops` | `<=>`    | `cosineDistance` | `ASC`  |
| `vector_ip_ops`     | `<#>`    | `dotProduct`     | `DESC` |

Each class has one `ORDER BY` operator and two support functions:
`chdb.vector_distance_name(int2)`, which maps its strategy number to the
ClickHouse function, and `chdb.vector_query_settings(int2)` below. That is
how the access method recognizes a vector operator class: it reads the
class's support function 2 from the catalogs and calls it with the strategy
of the class's one ordering operator, and never links against chdb_vector.
Vector operators are usable only in `ORDER BY ... LIMIT`.

## Settings

Each setting is a ClickHouse query setting of the vector search, sent with
every search; any role may set them.

### `chdb_vector.hnsw_candidate_list_size`

```sql
SET chdb_vector.hnsw_candidate_list_size = 512;
```

The number of candidates an HNSW search examines, ClickHouse's
`hnsw_candidate_list_size_for_search`: higher is slower and more accurate.
From `1` to `100000`; defaults to `256`.

### `chdb_vector.rescoring`

Whether the candidates are rescored by their exact distances, computed from
the stored vectors, ClickHouse's `vector_search_with_rescoring`; without it
the distances the index returns order the rows. Defaults to `off`. A
`dotProduct` search rescores whatever this says; see below.

### `chdb_vector.filter_strategy`

How a `WHERE` predicate combines with the HNSW search, ClickHouse's
`vector_search_filter_strategy`: `postfilter` searches the index first and
filters its candidates, `prefilter` filters first and searches the rows that
pass by brute force, and `auto` leaves the choice to ClickHouse. Defaults to
`auto`.

### `chdb.vector_query_settings`

```sql
SELECT chdb.vector_query_settings(2::int2);
```

Returns the settings above as a ClickHouse `SETTINGS` fragment for a search
by the given strategy, for the index scan and CustomScan to append to their
queries. A `dotProduct` search (strategy 3) always rescores: without
rescoring ClickHouse 26.9 sorts by the inner-product distance the index
returns, `1 - dot`, which the descending sort inverts.

## Limitations

*   ClickHouse builds an HNSW graph per data part, so a search returns the
    best candidates of each part and merges them; recall depends on the
    candidate list size and rescoring, and merges rebuild graphs.
*   An index scan returns at most `max_limit_for_vector_search_queries`
    rows (default 1000), the most ClickHouse's index serves; a larger
    `LIMIT` wants a sequential scan.
*   `dotProduct` is a similarity, so `<#>` orders DESC by `dotProduct`;
    the returned distance is its negation, as in pgvector.
*   A vector column indexed by `chdb` cannot hold NULL.
*   Only `vector` is supported, not `halfvec`, `bit` or `sparsevec`.

## Authors

*   [David E. Wheeler](https://justatheory.com/)
*   [serprex](https://github.com/serprex)

## Copyright

Copyright (c) 2026, ClickHouse

  [pgvector]: https://github.com/pgvector/pgvector
    "Open-source vector similarity search for Postgres"
  [chdb_search]: ./chdb_search.md "chdb_search Docs"
  [custom scan]: ./chdb_search-queries.md#the-custom-scan
    "chdb_search Queries: The Custom Scan"
