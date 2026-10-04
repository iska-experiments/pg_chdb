chdb_vector 0.1
===============

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

`chdb_vector` adds [pgvector] `vector` support to the `chdb` index access
method. A `vector(n)` column becomes an `Array(Float32)` column in ClickHouse
with a `vector_similarity('hnsw', <function>, n)` skip index. The column must
have a dimension: `vector` without a typmod cannot be indexed. Values are
encoded through pgvector's `vector::real[]` cast; a NULL vector cannot be
stored, as ClickHouse wants every array at the index's dimension.

An index scan ordered by a distance operator sends one query in the shape
ClickHouse's HNSW index serves, `ORDER BY <function>(col, q) LIMIT n`, with
`n` the server's `max_limit_for_vector_search_queries` (1000), the most the
index returns: as a pgvector scan returns at most `hnsw.ef_search` rows, a
`LIMIT` above that gets those rows only. The settings below go with it.

## Operator classes

| Class | Operator | ClickHouse | Order |
|---|---|---|---|
| `vector_l2_ops` | `<->` | `L2Distance` | ASC |
| `vector_cosine_ops` | `<=>` | `cosineDistance` | ASC |
| `vector_ip_ops` | `<#>` | `dotProduct` | DESC |

Each class has one ORDER BY operator and two support functions:
`chdb.vector_distance_name(int2)`, which maps its strategy number to the
ClickHouse function, and `chdb.vector_query_settings(int2)` below. Vector
operators are usable only in `ORDER BY ... LIMIT`.

## Settings

| Setting | Default | ClickHouse setting |
|---|---|---|
| `chdb_vector.hnsw_candidate_list_size` | 256 | `hnsw_candidate_list_size_for_search` |
| `chdb_vector.rescoring` | off | `vector_search_with_rescoring` |
| `chdb_vector.filter_strategy` | auto | `vector_search_filter_strategy` (`auto`, `postfilter`, `prefilter`) |

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

  [pgvector]: https://github.com/pgvector/pgvector
