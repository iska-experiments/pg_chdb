-- The search queries, each in the same SQL shape for both engines: the
-- chdb index on hn (operators in the chdb schema, so run with
-- search_path = chdb, public) and the ParadeDB index on hn_pdb.
-- bench.sh splits the file at the "-- q:" markers and times each query
-- with pgbench; psql -f runs it as is.
--
-- Operators: chdb @@@ (all tokens), @@? (any token), @@~ (phrase);
-- ParadeDB &&& (match conjunction), ||| (match disjunction), ### (phrase).

-- q: term_common chdb
SELECT id FROM hn WHERE text @@@ 'google';
-- q: term_common pdb
SELECT id FROM hn_pdb WHERE text &&& 'google';

-- The same, with a column neither index holds, so both read the heap: the
-- query above lets ParadeDB answer from its columnar copy of the key.
-- q: term_common_heap chdb
SELECT id, "by" FROM hn WHERE text @@@ 'google';
-- q: term_common_heap pdb
SELECT id, "by" FROM hn_pdb WHERE text &&& 'google';

-- q: term_rare chdb
SELECT id FROM hn WHERE text @@@ 'postgres';
-- q: term_rare pdb
SELECT id FROM hn_pdb WHERE text &&& 'postgres';

-- q: conjunction chdb
SELECT id FROM hn WHERE text @@@ 'google privacy';
-- q: conjunction pdb
SELECT id FROM hn_pdb WHERE text &&& 'google privacy';

-- q: disjunction chdb
SELECT id FROM hn WHERE text @@? 'postgres mysql';
-- q: disjunction pdb
SELECT id FROM hn_pdb WHERE text ||| 'postgres mysql';

-- q: phrase chdb
SELECT id FROM hn WHERE text @@~ 'open source';
-- q: phrase pdb
SELECT id FROM hn_pdb WHERE text ### 'open source';

-- q: count chdb
SELECT count(*) FROM hn WHERE text @@@ 'google';
-- q: count pdb
SELECT count(*) FROM hn_pdb WHERE text &&& 'google';

-- q: top10_score chdb
SELECT id, chdb.score(id) FROM hn WHERE text @@? 'google privacy'
 ORDER BY chdb.score(id) DESC LIMIT 10;
-- q: top10_score pdb
SELECT id, pdb.score(id) FROM hn_pdb WHERE text ||| 'google privacy'
 ORDER BY pdb.score(id) DESC LIMIT 10;

-- q: top10_score_time chdb
SELECT id, chdb.score(id) FROM hn WHERE text @@? 'google privacy'
   AND time >= '2020-01-01 00:00:00+00'
 ORDER BY chdb.score(id) DESC LIMIT 10;
-- q: top10_score_time pdb
SELECT id, pdb.score(id) FROM hn_pdb WHERE text ||| 'google privacy'
   AND time >= '2020-01-01 00:00:00+00'
 ORDER BY pdb.score(id) DESC LIMIT 10;

-- q: limit10 chdb
SELECT id FROM hn WHERE text @@@ 'google' LIMIT 10;
-- q: limit10 pdb
SELECT id FROM hn_pdb WHERE text &&& 'google' LIMIT 10;
