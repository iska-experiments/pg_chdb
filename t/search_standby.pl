#!/usr/bin/perl

# The store lives in the index relation's pages, so it travels with the
# relation: a streaming standby has it, a promoted standby serves it with no
# REINDEX, and the engine's own directory under pg_chdb is a cache a worker
# rebuilds from the catalog and the pages when it starts. A server in
# recovery does not serve the index yet: the access method refuses it as
# chdb_search.unavailable_index says, an error by default, or in skip mode
# a plan without the index that answers from the heap.

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

my $primary = search_node('primary', { allows_streaming => 1 });
END { $primary->stop if $primary }

my $dboid = $primary->safe_psql(postgres =>
    "SELECT oid FROM pg_database WHERE datname = 'postgres'");
my $cache = $primary->data_dir . "/pg_chdb/pgsql_tmp/$dboid";
my $skip  = 'SET chdb_search.unavailable_index = skip;';

# A search through the index on a node: its rows, or psql's stderr.
sub search {
    my ($node, $pre) = @_;
    my ($out, $err) = ('', '');
    $node->psql(postgres =>
        ($pre // '') . " SET enable_seqscan = off; SELECT id FROM docs WHERE body @@@ 'boots' ORDER BY id",
        stdout => \$out, stderr => \$err);
    return $err || $out;
}

is search($primary), 2, 'The primary should answer a search';

# A standby from a base backup, streaming.
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;
END { $standby->stop if $standby }

# Rows committed after the backup reach the standby's index pages.
$primary->safe_psql(postgres => "INSERT INTO docs VALUES (3, 'Hiking boots')");
is search($primary), "2\n3", 'The primary should answer with the new row';
$primary->wait_for_catchup($standby);

# In recovery the index is refused, or skipped for a plan without it.
my $err = search($standby);
like $err, qr/ERROR:\s+chdb index "docs_idx" is not available on this server/,
    'A standby should refuse the index';
like $err, qr/DETAIL:\s+Its store is not served on a server in recovery\./,
    'Should say why';
like $standby->safe_psql(postgres =>
    "$skip EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'boots'"),
    qr/Seq Scan/, 'Skip mode should plan without the index';
is $standby->safe_psql(postgres =>
    "$skip SELECT id FROM docs WHERE body @@@ 'boots' ORDER BY id"), "2\n3",
    'Skip mode should answer from the heap';
ok !-e $standby->data_dir . "/pg_chdb/pgsql_tmp/$dboid",
    'The standby should have started no worker';

# Promoted, the standby serves the index from the replicated pages at once.
$standby->promote;
$standby->poll_query_until(postgres => 'SELECT NOT pg_is_in_recovery()')
    or die 'the standby did not promote';
is search($standby), "2\n3", 'The promoted standby should answer with every row';
$standby->safe_psql(postgres => "INSERT INTO docs VALUES (4, 'Climbing boots')");
is search($standby), "2\n3\n4", 'The promoted standby should take new rows';
ok $standby->log_contains(qr/chdb_search attach: ATTACH TABLE IF NOT EXISTS idx_\d+\.t_\d+ UUID/),
    'The new worker should attach the table from the catalog';
$standby->stop;

# The engine's directory on the primary is a cache: removed, it is rebuilt.
$primary->stop;
ok -d $cache, 'The primary should have an engine directory';
rmtree($cache);
$primary->start;
is search($primary), "2\n3", 'The index should answer without its engine directory';
ok -d $cache, 'The worker should have made the directory again';

# A REINDEX is never needed, but still works.
$primary->safe_psql(postgres => 'REINDEX INDEX docs_idx');
is search($primary), "2\n3", 'REINDEX should keep the index answering';

done_testing;
