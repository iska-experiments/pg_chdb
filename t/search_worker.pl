#!/usr/bin/perl

# The chdb_search worker is a background worker that attaches to shared
# memory, and libchdb runs in a child of it, the engine. A signal death of
# the engine costs the request in flight: the worker reports the signal in
# its answer and starts another engine for the next request, and Postgres
# never notices. A signal death of the worker itself is a crash of the
# instance, as of any backend: the postmaster runs crash recovery, the engine
# dies with its parent, and the next call finds a new worker over the same
# store.

use v5.34;
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

plan skip_all => 'needs the chdb_search worker' if $ENV{CHDB_SEARCH_STUB};

# The node's default is to shut down on a crash; let it recover instead.
my $node = search_node('worker', 'restart_after_crash = on');
END { $node->stop('immediate') if $node }

# A search answered by the worker from the index's store: its rows, or psql's
# stderr when it fails.
sub search {
    my ($out, $err) = ('', '');
    $node->psql(postgres => q{SET enable_seqscan = off; SELECT id FROM docs WHERE body @@@ 'boots'},
        stdout => \$out, stderr => \$err);
    return $err || $out;
}

# The stderr of a statement, '' when it succeeds.
sub stderr_of {
    my ($out, $err) = ('', '');
    $node->psql(postgres => shift, stdout => \$out, stderr => \$err);
    return $err;
}

sub engine_pid {
    return $node->safe_psql(postgres => 'SELECT chdb_search_engine_pid()');
}

is search(), 2, 'The index should answer a search';

my $worker = worker_pid($node, 'postgres');
ok $worker, 'Should have a worker listed in pg_stat_activity';
my $dboid = $node->safe_psql(postgres => "SELECT oid FROM pg_database WHERE datname = 'postgres'");
ok -S $node->data_dir . "/pg_chdb/$dboid.sock",
    'Should have a socket under pg_chdb in the data directory';
ok engine_pid(), 'Should have an engine serving the worker';

# The engine killed by a signal: the request that finds it dead fails naming
# the signal, the next one is served by a new engine, and the worker and the
# instance go on as they were.
for my $signal (9, 11) {
    subtest "engine killed by signal $signal" => sub {
        my $offset = -s $node->logfile;
        my $old    = engine_pid();

        is $node->safe_psql(postgres => "SELECT chdb_search_debug_kill_engine($signal)"),
            $old, 'Should signal the engine';
        like search(), qr/chDB engine \(pid $old\) was terminated by signal $signal\b/,
            'The request that finds the engine dead should fail naming the signal';
        is search(), 2, 'The next request should be served';

        my $new = engine_pid();
        ok $new && $new ne $old, 'Should be served by a new engine';
        is worker_pid($node, 'postgres'), $worker, 'The worker should be the same';
        ok $node->log_contains(
            qr/chdb_search: chDB engine \(pid $old\) was terminated by signal $signal\b/, $offset),
            'The worker should log the engine death';
        unlike slurp_file($node->logfile, $offset),
            qr/terminating any other active server processes/,
            'The instance should not restart';
    };
}

# The worker killed by a signal: a crash of the instance. After recovery a
# new worker serves the same store; that its engine can open the store shows
# the old engine died with its parent and released the store's lock.
for my $signal (qw(KILL SEGV)) {
    subtest "worker killed by SIG$signal" => sub {
        my $offset = -s $node->logfile;
        my $old    = worker_pid($node, 'postgres');
        ok $old, 'Should find the worker';

        is kill($signal, $old), 1, 'Should signal the worker';
        ok $node->wait_for_log(qr/terminating any other active server processes/, $offset),
            'Should restart the instance';
        ok $node->log_contains(
            qr/chdb_search worker\b.*\(PID $old\) was terminated by signal/, $offset),
            'Should log the worker terminated by the signal';
        ok $node->poll_query_until('postgres', undef, ''),
            'Should accept connections after recovery';
        ok $node->log_contains(qr/database system was not properly shut down/, $offset),
            'Should have run crash recovery';

        is search(), 2, 'Should serve the next call after recovery';
        my $new = worker_pid($node, 'postgres');
        ok $new && $new ne $old, 'Should be served by a new worker';
    };
}

# DROP DATABASE has to disconnect every session of the database, and a
# background worker is one. See what it does with a running worker.
$node->safe_psql(postgres => 'CREATE DATABASE doomed');
$node->safe_psql(doomed => q{
    CREATE EXTENSION chdb_search;
    CREATE TABLE t (body text);
    CREATE INDEX ON t USING chdb (body);
});
ok worker_pid($node, 'doomed'), 'Should have a worker for the second database';

{
    local $TODO = 'DROP DATABASE does not stop the database\'s chdb_search worker';
    is stderr_of('DROP DATABASE doomed'), '', 'Should drop a database with a running worker';
}

done_testing;
