#!/usr/bin/perl

# Stores and store directories whose owner went without the worker hearing of
# it, and their removal by the worker when it next starts.

use v5.34;
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

my $node = search_node('sweep');
END { $node->stop if $node }

my $pg_chdb = $node->data_dir . '/pg_chdb';

# Whether the store of index $oid exists, asked of the worker, which starts
# one if none runs and so runs the sweep first.
sub has_store {
    my $oid = shift;
    return $node->safe_psql(postgres => qq{
        SELECT * FROM chdb_search_query(
            'SELECT count() FROM system.databases WHERE name = ''idx_$oid'''
        ) AS (n bigint)
    });
}

# Stops the worker of database $db, so that the next request starts a new one.
sub stop_worker {
    my $db = shift;
    $node->safe_psql(postgres => qq{
        SELECT pg_terminate_backend(pid) FROM pg_stat_activity
         WHERE backend_type = 'chdb_search worker' AND datname = '$db'
    });
    $node->poll_query_until(postgres => qq{
        SELECT count(*) = 0 FROM pg_stat_activity
         WHERE backend_type = 'chdb_search worker' AND datname = '$db'
    }) or die "the worker of $db did not stop";
}

ORPHAN_STORE: {
    # A store named after a relation that is no chdb index, as a dropped
    # index's is once its OID is gone or reused.
    my $oid = $node->safe_psql(postgres => q{
        CREATE TABLE t (i int);
        SELECT 't'::regclass::oid;
    });
    $node->safe_psql(postgres =>
        "SELECT chdb_search_exec('CREATE DATABASE idx_$oid')");
    is has_store($oid), 1, 'The store should exist while its worker runs';

    # A build in progress holds its OID exclusively before the catalog shows
    # the index, so a store whose OID cannot be share-locked is kept.
    my $holder = $node->background_psql('postgres');
    $holder->query_safe('BEGIN; LOCK TABLE t IN ACCESS EXCLUSIVE MODE');
    stop_worker('postgres');
    is has_store($oid), 1, 'A store whose OID is locked should be kept';
    $holder->query_safe('COMMIT');
    $holder->quit;

    my $offset = -s $node->logfile;
    stop_worker('postgres');
    is has_store($oid), 0, 'The restarted worker should sweep the store';
    ok $node->log_contains(qr/chdb_search: swept orphan store idx_$oid\b/, $offset),
        'Should log the sweep';
    is has_store(0), 1, 'The debug database should stay';
}

DROPPED_DATABASE: {
    $node->safe_psql(postgres => 'CREATE DATABASE d2');
    $node->safe_psql(d2 => q{
        CREATE EXTENSION chdb_search;
        SELECT chdb_search_exec('SELECT 1');
    });
    my $dboid = $node->safe_psql(postgres =>
        "SELECT oid FROM pg_database WHERE datname = 'd2'");
    ok -d "$pg_chdb/$dboid", 'd2 should have a store directory';

    # Its worker holds the database open; stop it so that it can be dropped.
    # Nothing tells the worker of another database about the drop.
    stop_worker('d2');
    $node->safe_psql(postgres => 'DROP DATABASE d2');
    ok -d "$pg_chdb/$dboid", 'The directory should survive the DROP DATABASE';
    # A worker that crashed would have left its socket too.
    open my $sock, '>', "$pg_chdb/$dboid.sock" or die $!;
    close $sock;

    my $offset = -s $node->logfile;
    stop_worker('postgres');
    has_store(0);
    ok !-e "$pg_chdb/$dboid", 'The next worker to start should remove the directory';
    ok !-e "$pg_chdb/$dboid.sock", 'The socket should be gone too';
    ok $node->log_contains(
        qr/chdb_search: sweeping store of dropped database $dboid\b/, $offset),
        'Should log the sweep';
}

done_testing;
