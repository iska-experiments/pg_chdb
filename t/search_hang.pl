#!/usr/bin/perl

# The worker serves one request at a time, and a store drop runs in the
# transaction's commit callback, where Postgres holds interrupts: a drop that
# found the worker busy with another backend's build or OPTIMIZE, or stopped
# as here, hung the COMMIT, holding AccessExclusiveLock on the table, beyond
# pg_cancel_backend and statement_timeout until the worker answered. The
# client now bounds such a wait by chdb_search.worker_timeout: the drop warns
# and the store is swept when the worker next starts.

use v5.34;
use strict;
use warnings FATAL => 'all';
use Time::HiRes qw(time);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

plan skip_all => 'needs the chdb_search worker' if $ENV{CHDB_SEARCH_STUB};

my $node = search_node('hang',
    "session_preload_libraries = 'chdb_search'", 'chdb_search.worker_timeout = 3');
END { $node->stop if $node }

my $oid = $node->safe_psql(postgres => "SELECT 'docs_idx'::regclass::oid");
my $worker = $node->safe_psql(postgres => q{
    SELECT pid FROM pg_stat_activity
     WHERE backend_type = 'chdb_search worker' AND datname = 'postgres'
});
ok $worker, 'The worker should be running';

# Stopped, the worker still accepts connections into its backlog and never
# answers them, as it would not while serving a long request.
is kill('STOP', $worker), 1, 'Should stop the worker';
my $start = time;
my ($out, $err) = ('', '');
$node->psql(postgres => 'DROP INDEX docs_idx', stdout => \$out, stderr => \$err);
my $took = time - $start;
like $err,
    qr/WARNING:\s+chdb_search: could not clean up index $oid: chdb_search: timed out waiting for the worker/,
    'The drop should give up on the worker with a warning';
ok $took < 20, "The DROP INDEX should return within the timeout (took ${took}s)";
is $node->safe_psql(postgres => "SELECT count(*) FROM pg_class WHERE oid = $oid"), 0,
    'The index should be dropped';

# Once the worker runs again the store goes, by the late request or the sweep.
is kill('CONT', $worker), 1, 'Should continue the worker';
$node->restart;
is $node->safe_psql(postgres => qq{
    SELECT * FROM chdb_search_query(
        'SELECT count() FROM system.databases WHERE name = ''idx_$oid'''
    ) AS (n bigint)
}), 0, 'The orphaned store should be swept';

done_testing;
