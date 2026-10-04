#!/usr/bin/perl

# A VACUUM whose dead TIDs overflow maintenance_work_mem vacuums the index in
# several rounds, each of which reads the whole store. The rows seen were
# summed across rounds, so the index's reltuples came out at several times
# its live rows and the OPTIMIZE ratio, dead over seen, was divided by the
# rounds, skipping the merge on exactly the heavy-churn vacuums that need it.

use v5.34;
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

plan skip_all => 'needs the chdb_search worker' if $ENV{CHDB_SEARCH_STUB};

my $node = search_node('rounds');
END { $node->stop if $node }

$node->safe_psql(postgres => q{
    INSERT INTO docs SELECT 100 + i, 'word ' || i FROM generate_series(1, 200000) i;
    DELETE FROM docs WHERE id % 2 = 0;
});

my $offset = -s $node->logfile;
my ($out, $err) = ('', '');
$node->psql(postgres => q{
    SET maintenance_work_mem = '64kB';
    SET chdb_search.vacuum_optimize_ratio = 0.4;
    VACUUM (VERBOSE) docs;
}, stdout => \$out, stderr => \$err);
my ($rounds) = $err =~ /index scans: (\d+)/;
ok defined $rounds && $rounds >= 2,
    'VACUUM should take several index rounds (got ' . ($rounds // 'none') . ')';
like $err, qr/index "docs_idx": pages: 1 in total/, 'Should report the index page';

my $live = $node->safe_psql(postgres => 'SELECT count(*) FROM docs');
is $node->safe_psql(postgres =>
    "SELECT reltuples::bigint FROM pg_class WHERE relname = 'docs_idx'"), $live,
    'reltuples should be the live rows, not the rows summed over the rounds';
ok $node->log_contains(qr/chdb_search exec: OPTIMIZE TABLE idx_\d+\.t_\d+ FINAL/, $offset),
    'Half the store dead should be merged, whatever the rounds';

done_testing;
