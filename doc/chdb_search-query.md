chdb_search Query Language
==========================

How to ask a [chdb_search](chdb_search.md) index for rows: the operators
over tokens and patterns, and the `chdb.query` type that combines them. All
functions and the type live in the `chdb` schema; put it on the
`search_path`, or write `OPERATOR(chdb.@@@)` and `chdb.term(...)`.

## Operators

Each operator has a Postgres implementation, so a sequential scan and the
heap recheck return the same rows as the index, and a ClickHouse
translation the index uses.

| Operator          | Function                          | ClickHouse            |
| ----------------- | --------------------------------- | --------------------- |
| `col @@@ 'a b'`   | `chdb.has_all_tokens(col, 'a b')` | `hasAllTokens`        |
| `col @@? 'a b'`   | `chdb.has_any_tokens(col, 'a b')` | `hasAnyTokens`        |
| `col @@= 'a'`     | `chdb.has_token(col, 'a')`        | `hasToken`            |
| `col @@~ 'a b'`   | `chdb.has_phrase(col, 'a b')`     | `hasPhrase`           |
| `col @@/ 're'`    | `chdb.regex(col, 're')`           | `match`               |
| `col @@% 'pat%'`  | `chdb.wildcard(col, 'pat%')`      | `LIKE`                |
| `col @@@ query`   | `chdb.query_matches(col, query)`  | the tree, see below   |
| none              | `chdb.tokens(text)`               | `tokens`              |

The token operators and `@@@ query` also take a `text[]` left argument,
where each element is one token and the needle is one token too, so all,
any and one mean the same; a phrase or a pattern over elements has no
meaning and is refused. `@@~` needs `support_phrase_search` on the column.
`chdb.tokens()` shows what the default tokenizer, run in the worker, makes
of a string.

```sql
SELECT id FROM docs WHERE body @@@ 'postgres clickhouse'; -- both, any order
SELECT id FROM docs WHERE body @@? 'postgres clickhouse'; -- at least one
SELECT id FROM docs WHERE body @@= 'postgres';            -- one token
SELECT id FROM docs WHERE body @@~ 'full text search';    -- in order
SELECT id FROM docs WHERE body @@/ 'postgres(ql)?';       -- regular expression
SELECT id FROM docs WHERE body @@% 'postgres%';           -- LIKE pattern
```

Matching on tokens is exact: `postgres` does not match `postgresql`, and a
needle without tokens matches nothing.

### Regex and Wildcard

`@@/` and `@@%` read the text rather than the posting lists. They follow
the column's preprocessor: with the default `lowerUTF8` the text is
lowercased and the pattern matched without regard to case, so `@@/ 'RUN'`
and `@@% 'Run%'` find `running`; with `preprocessor = 'none'` they match
case-sensitively, with `extractTextFromHTML` they match the extracted text.
In ClickHouse that is `match(lowerUTF8(col), '(?i)re')` and
`lowerUTF8(col) LIKE lowerUTF8('pat%')`.

A regular expression is [RE2's][re2] in ClickHouse and an [ARE] in Postgres.
They agree on the everyday syntax and part at the edges: lookahead is ARE
only, `\b` is RE2 only, and case folds by Unicode in RE2 but by the
collation's ctype in Postgres, which under `C` folds ASCII alone (a
lowercase pattern is unaffected, as the text is lowercased first). In a
wildcard pattern `%` matches any string, `_` one character, and a backslash
escapes `%`, `_` and itself; before any other character ClickHouse keeps the
backslash while Postgres drops it.

The text index of ClickHouse 26.9 prunes no granules for `match()` nor for
a `LIKE` over a preprocessed column (and a raw `LIKE` only by whole tokens
between wildcards), so a pattern alone reads the store's column. Beside a
token predicate it reads only the rows the index narrowed to:

```sql
SELECT id FROM docs WHERE body @@@ 'running' AND body @@/ 'run+ing';
```

## Queries

The scan keys an index receives are ANDed, so the operators above cannot
say "these but not those" or weight one term. The `chdb.query` type holds a
search as a tree, built by functions named as the ClickHouse functions are,
and `col @@@ query` searches a column for it:

| Builder                        | Matches rows whose column                    |
| ------------------------------ | -------------------------------------------- |
| `chdb.match('a b')`            | has any of the tokens (`hasAnyTokens`)        |
| `chdb.match_all('a b')`        | has all of the tokens (`hasAllTokens`)        |
| `chdb.term('a')`               | has the token (`hasToken`)                   |
| `chdb.phrase('a b'[, slop])`   | has the tokens in order, see below           |
| `chdb.regex('re')`             | matches the regular expression               |
| `chdb.wildcard('pat%')`        | matches the LIKE pattern                     |
| `q1 && q2`, `chdb.all_of(...)` | matches every query                          |
| `q1 \|\| q2`, `chdb.any_of(...)` | matches any query                          |
| `!q`, `chdb.none_of(q)`        | does not match the query                     |
| `chdb.boost(q, weight)`        | matches `q`; the weight is for the score     |
| `chdb.in_column(q, 'name')`    | matches `q` in another column of the index   |

```sql
SELECT id FROM docs
 WHERE body @@@ (chdb.match_all('running shoes') && !chdb.term('boots'));
SELECT id FROM docs
 WHERE body @@@ chdb.any_of(chdb.phrase('trail running', 1),
                            chdb.boost(chdb.regex('^trail'), 2));
```

A query prints, and reads back, as the calls that build it, which is what
`EXPLAIN` shows and what a prepared statement's parameter can hold:

```sql
SELECT chdb.match_all('running shoes') && !chdb.term('boots');
-- and(match_all('running shoes'), not(term('boots')))
SELECT 'or(term(''a''), phrase(''b c'', 1))'::chdb.query;
```

The index evaluates the tree as one ClickHouse expression, each leaf as its
operator renders it. The Postgres implementation walks the tree with the
same per-leaf implementations. `boost` changes nothing in the filter; it
multiplies the weight [`chdb.score()`](chdb_search-queries.md#relevance-score)
gives the tokens of the leaves below it. A NULL text never matches, a `!`
included,
as the operator is strict; a NULL `text[]` reads as the empty array, which
is what the index stores for it, so `tags @@@ !chdb.term('x')` matches it.

`chdb.in_column(q, 'name')` searches another indexed column, for a query
over several columns through one operator; such a query is answered through
the index only, and a sequential scan raises.

### Phrase Slop

`chdb.phrase('running shoes', 2)` matches rows where `running` comes before
`shoes` with at most two other tokens between them: `running b c shoes`
matches, `running b c d shoes` does not, nor does `shoes running` at any
slop, as the order is never relaxed. Slop 0 is `@@~`. ClickHouse stores
token positions only with `support_phrase_search`, so a phrase with slop is
`hasAllTokens`, which the index answers, and a check of the positions in
`tokens(col)` computed with the column's tokenizer and preprocessor, which
reads the rows the index narrowed to.

## The Postgres Implementations

The Postgres implementations know ClickHouse's default pipeline only,
`lowerUTF8` then `splitByNonAlpha`, lowercasing by Unicode whatever the
cluster's locale. An index built with another tokenizer or preprocessor
answers differently: `preprocessor = 'none'` matches patterns
case-sensitively through the index and without regard to case by sequential
scan. For such a column the operators are meaningful through the index
only, which the planner takes once the table has rows enough; a test can
force it with `SET enable_seqscan = off`.

  [re2]: https://github.com/google/re2/wiki/Syntax "RE2 Syntax"
  [ARE]: https://www.postgresql.org/docs/current/functions-matching.html#POSIX-SYNTAX-DETAILS
    "PostgreSQL Docs: Regular Expression Details"
