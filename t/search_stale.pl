#!/usr/bin/perl

# A store older than the heap, as a restore from a backup, pg_rewind or a copy
# leaves it, and a store that is gone. The access method compares the store's
# record of its last flush with the index's metapage before a scan, before a
# commit flushes to the store and before VACUUM's deletes, and refuses a store
# it cannot prove current as chdb_search.unavailable_index says: an error by
# default, or no index path and no flush, with the index moved on so that the
# store can never match it again short of a REINDEX.

use v5.34;
use strict;
use warnings FATAL => 'all';
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

plan skip_all => 'needs the chdb_search worker' if $ENV{CHDB_SEARCH_STUB};

my $node = search_node('stale');
END { $node->stop if $node }

my $dboid = $node->safe_psql(postgres =>
    "SELECT oid FROM pg_database WHERE datname = 'postgres'");
my $store = $node->data_dir . "/pg_chdb/$dboid";
my $skip = 'SET chdb_search.unavailable_index = skip;';

sub search {
    my $pre = shift // '';
    return $node->safe_psql(postgres =>
        "$pre SET enable_seqscan = off; SELECT id FROM docs WHERE body @@@ 'boots' ORDER BY id");
}

sub search_fails {
    return stderr_of($node, "SET enable_seqscan = off; SELECT id FROM docs WHERE body @@@ 'boots'");
}

# Swaps the store directory for another copy while the server is down.
sub swap_store {
    my ($aside, $back) = @_;
    $node->stop;
    rename($store, $aside) or die "rename $store: $!";
    rename($back, $store) or die "rename $back: $!";
    $node->start;
}

is search(), 2, 'The index should answer a search';

# Keep a copy of the store, then commit a row, which flushes to the live one.
$node->stop;
PostgreSQL::Test::Utils::system_or_bail('cp', '-a', $store, "$store.old");
$node->start;
$node->safe_psql(postgres => "INSERT INTO docs VALUES (3, 'Hiking boots')");
is search(), "2\n3", 'The index should answer with the new row';

# Put the old copy back: the heap and the metapage are ahead of the store.
swap_store("$store.new", "$store.old");
my $err = search_fails();
like $err, qr/ERROR:\s+chdb index "docs_idx" is not available on this server/,
    'A store behind the index should be refused';
like $err, qr/DETAIL:\s+The store was last flushed at [0-9A-F]+\/[0-9A-F]+, the index at/,
    'Should say the two flush positions differ';
like $err, qr/HINT:\s+REINDEX INDEX "docs_idx" rebuilds its store\./,
    'Should name REINDEX';

# In skip mode the planner takes another path, and the answer is right.
like $node->safe_psql(postgres =>
    "$skip EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'boots'"),
    qr/Seq Scan/, 'Skip mode should plan without the index';
is $node->safe_psql(postgres =>
    "$skip SELECT id FROM docs WHERE body @@@ 'boots' ORDER BY id"), "2\n3",
    'Skip mode should answer from the heap';

# A commit does not flush to a store it cannot trust: flushing would record
# the flush on both sides and pass the stale store as current. In error mode
# the commit is refused; in skip mode it keeps its rows from the store and
# moves the index on.
like stderr_of($node, "INSERT INTO docs VALUES (4, 'Climbing boots')"),
    qr/ERROR:\s+chdb index "docs_idx" is not available on this server/,
    'A commit into a stale store should be refused';
is $node->safe_psql(postgres => 'SELECT count(*) FROM docs WHERE id = 4'), 0,
    'The refused commit should leave no row';
my $offset = -s $node->logfile;
is stderr_of($node, "$skip INSERT INTO docs VALUES (4, 'Climbing boots')"), '',
    'Skip mode should commit without the store';
is $node->safe_psql(postgres => 'SELECT count(*) FROM docs WHERE id = 4'), 1,
    'The skipped commit should keep its row';
ok !$node->log_contains(qr/chdb_search insert: INSERT INTO idx_/, $offset),
    'The skipped commit should flush nothing';
like search_fails(), qr/not available on this server/,
    'The store should stay refused after the skipped commit';

# The copy that matched the index before the restore no longer does: the
# skipped commit moved the index past every flush the store ever saw.
swap_store("$store.old", "$store.new");
like search_fails(), qr/DETAIL:\s+The store was last flushed at .*, the index at/,
    'A store put back by hand should stay refused once a commit was skipped';

# VACUUM will not delete from a store it cannot trust either. (It asks the
# index only when the heap has a dead tuple to remove.)
$node->safe_psql(postgres => 'DELETE FROM docs WHERE id = 1');
like stderr_of($node, 'VACUUM docs'), qr/chdb index "docs_idx" is not available on this server/,
    'VACUUM should refuse the stale store';

# REINDEX rebuilds the store from the heap, with the rows of the skipped commit.
$node->safe_psql(postgres => 'REINDEX INDEX docs_idx');
is search(), "2\n3\n4", 'REINDEX should make the index available again';

# A store that is gone altogether: the worker makes an empty one, which has
# no record of the index's generation.
$node->stop;
rmtree($store);
$node->start;
$err = search_fails();
like $err, qr/ERROR:\s+chdb index "docs_idx" is not available on this server/,
    'A missing store should be refused';
like $err, qr/DETAIL:\s+The store has no table for generation \d+\./,
    'Should say the generation is missing';
$node->safe_psql(postgres => 'REINDEX INDEX docs_idx');
is search(), "2\n3\n4", 'REINDEX should rebuild the missing store';

done_testing;
