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
encoded through pgvector's `vector::real[]` cast.

## Operator classes

| Class | Operator | ClickHouse | Order |
|---|---|---|---|
| `vector_l2_ops` | `<->` | `L2Distance` | ASC |
| `vector_cosine_ops` | `<=>` | `cosineDistance` | ASC |
| `vector_ip_ops` | `<#>` | `dotProduct` | DESC |

Each class has one ORDER BY operator and the support function
`chdb.vector_distance_name(int2)`, which maps its strategy number to the
ClickHouse function. Vector operators are usable only in `ORDER BY ... LIMIT`.

The operator classes are created only if the `chdb` access method exists
when `chdb_vector` is created; otherwise drop and recreate the extension.

## Settings

| Setting | Default | ClickHouse setting |
|---|---|---|
| `chdb_vector.hnsw_candidate_list_size` | 256 | `hnsw_candidate_list_size_for_search` |
| `chdb_vector.rescoring` | off | `vector_search_with_rescoring` |
| `chdb_vector.filter_strategy` | auto | `vector_search_filter_strategy` (`auto`, `postfilter`, `prefilter`) |

### `chdb.vector_query_settings`

```sql
SELECT chdb.vector_query_settings();
```

Returns the settings above as a ClickHouse `SETTINGS` fragment, for the
index scan and CustomScan to append to their queries.

## Limitations

*   ClickHouse builds an HNSW graph per data part, so a search returns the
    best candidates of each part and merges them; recall depends on the
    candidate list size and rescoring, and merges rebuild graphs.
*   ClickHouse rejects vector search above
    `max_limit_for_vector_search_queries` (default 100); larger `LIMIT`s
    cannot use the index.
*   `dotProduct` is a similarity, so `<#>` orders DESC by `dotProduct`;
    the returned distance is its negation, as in pgvector.
*   Only `vector` is supported, not `halfvec`, `bit` or `sparsevec`.

  [pgvector]: https://github.com/pgvector/pgvector
