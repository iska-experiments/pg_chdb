#!/usr/bin/perl

# The chdb_search worker is a background worker that attaches to shared
# memory, so unlike chdb_helper the postmaster treats its death by signal as a
# crash of the whole instance. Check that the instance recovers and the next
# call finds a fresh worker over the same store, and that a database with a
# running worker can be dropped.

use v5.34;
use strict;
use warnings FATAL => 'all';
use Cwd qw(abs_path);
use File::Basename qw(dirname);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

plan skip_all => 'needs POSIX signals' if $^O eq 'MSWin32';

# The worker dlopens libchdb, so give the server a library path to find it on:
# an explicit LIBCHDB_LIB_DIR, else the copy the Makefile bundles under vendor.
my $root = abs_path(dirname(__FILE__) . '/..');
my ($libdir) = grep { defined && -d } (
    $ENV{LIBCHDB_LIB_DIR},
    glob("$root/vendor/libchdb-*/lib"),
);
$ENV{LD_LIBRARY_PATH} = join ':', grep { length }
    ($libdir // '', $ENV{LD_LIBRARY_PATH} // '');

my $node = PostgreSQL::Test::Cluster->new('search_worker');
$node->init;
$node->append_conf('postgresql.conf', "log_min_messages = DEBUG1\nrestart_after_crash = on\n");
$node->start;
END { $node->stop('immediate') if $node }

$node->safe_psql(postgres => 'CREATE EXTENSION chdb_search');

# Create a store with a few rows.
$node->safe_psql(postgres => q{
    SELECT chdb_search_drop();
    SELECT chdb_search_exec(
        'CREATE TABLE idx_0.t (id UInt64, body String) ENGINE = MergeTree ORDER BY id');
    SELECT chdb_search_exec(
        'INSERT INTO idx_0.t SELECT number, toString(number) FROM numbers(1000)');
});

my $count = q{SELECT n FROM chdb_search_query('SELECT count() FROM idx_0.t') AS (n bigint)};
is $node->safe_psql(postgres => $count), 1000, 'Should count the rows via the worker';

my $pid = worker_pid($node, 'postgres');
ok $pid, 'Should have a worker listed in pg_stat_activity';
ok -S $node->data_dir . '/pg_chdb/' . $node->safe_psql(
    postgres => q{SELECT oid FROM pg_database WHERE datname = 'postgres'}
) . '.sock', 'Should have a socket under pg_chdb in the data directory';

# A worker killed by a signal takes the instance through crash recovery.
for my $signal (qw(KILL SEGV)) {
    subtest "worker killed by SIG$signal" => sub {
        my $offset = -s $node->logfile;
        my $old    = worker_pid($node, 'postgres');
        ok $old, 'Should find the worker';

        is kill($signal, $old), 1, 'Should signal the worker';

        ok $node->wait_for_log(
            qr/terminating any other active server processes/, $offset),
            'Should restart the instance';
        ok $node->log_contains(
            qr/chdb_search worker\b.*\(PID $old\) was terminated by signal/, $offset),
            'Should log the worker terminated by the signal'
            or diag join "\n", grep { /PID $old|terminated|crash/ }
                split /\n/, slurp_file($node->logfile, $offset);

        ok $node->poll_query_until('postgres', undef, ''),
            'Should accept connections after recovery';
        my ($rc, $out, $err) = $node->psql(postgres => $count);
        is $rc, 0, 'Should serve the next call after recovery' or diag $err;
        is $out, 1000, 'Should find the same store with its rows';
        ok $node->log_contains(qr/database system was not properly shut down/, $offset)
            || $node->log_contains(qr/redo starts at/, $offset),
            'Should have run crash recovery';

        my $new = worker_pid($node, 'postgres');
        ok $new && $new ne $old, 'Should be served by a new worker';
    };
}

# DROP DATABASE has to disconnect every session of the database, and a
# background worker is one. See what it does with a running worker.
$node->safe_psql(postgres => 'CREATE DATABASE doomed');
$node->safe_psql(doomed => 'CREATE EXTENSION chdb_search');
$node->safe_psql(doomed => q{SELECT chdb_search_exec(
    'CREATE TABLE idx_0.t (id UInt64) ENGINE = MergeTree ORDER BY id')});
ok worker_pid($node, 'doomed'), 'Should have a worker for the second database';

{
    my $offset = -s $node->logfile;
    my ($rc, $out, $err) = $node->psql(postgres => 'DROP DATABASE doomed');
    local $TODO = 'DROP DATABASE does not stop the database\'s chdb_search worker';
    is $rc, 0, 'Should drop a database with a running worker'
        or diag "psql said: $err";
}

done_testing;
