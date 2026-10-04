#!/usr/bin/perl

# Stores and store directories whose owner went without the worker hearing of
# it, among them the drops the object access hook never saw because the
# dropping backend had not loaded chdb_search, and their removal by the worker
# when it next starts.

use v5.34;
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

plan skip_all => 'needs the chdb_search worker' if $ENV{CHDB_SEARCH_STUB};

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

# A chdb index in database $db, whose worker's directory then exists; the
# worker is stopped, so that the database can be dropped.
sub make_store {
    my $db = shift;
    $node->safe_psql(postgres => "CREATE DATABASE $db");
    $node->safe_psql($db => q{
        CREATE EXTENSION chdb_search;
        CREATE TABLE t (body text);
        INSERT INTO t VALUES ('x');
        CREATE INDEX ON t USING chdb (body);
    });
    my $dboid = $node->safe_psql(postgres =>
        "SELECT oid FROM pg_database WHERE datname = '$db'");
    ok -d "$pg_chdb/$dboid", "$db should have a store directory";
    stop_worker($node, $db);
    return $dboid;
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
    stop_worker($node, 'postgres');
    is has_store($oid), 1, 'A store whose OID is locked should be kept';
    $holder->query_safe('COMMIT');
    $holder->quit;

    my $offset = -s $node->logfile;
    stop_worker($node, 'postgres');
    is has_store($oid), 0, 'The restarted worker should sweep the store';
    ok $node->log_contains(qr/chdb_search: swept orphan store idx_$oid\b/, $offset),
        'Should log the sweep';
    is has_store(0), 1, 'The debug database should stay';
}

MISSED_DROP: {
    my $oid = $node->safe_psql(postgres => "SELECT 'docs_idx'::regclass::oid");
    is has_store($oid), 1, 'The index should have a store';

    # A fresh backend loads the library inside the DROP, after the hook for
    # the index would have fired.
    my $offset = -s $node->logfile;
    $node->safe_psql(postgres => 'DROP TABLE docs');
    ok $node->log_contains(
        qr/LOG:\s+chdb_search was loaded on demand by this DROP TABLE/, $offset),
        'Should say the library was loaded on demand';
    ok $node->log_contains(
        qr/HINT:\s+Add chdb_search to session_preload_libraries/, $offset),
        'Should hint at preloading';
    ok !$node->log_contains(qr/chdb_search drop: idx_$oid\b/, $offset),
        'Should have missed the drop';
    is has_store($oid), 1, 'Should leave the store behind';

    # The worker's next start sweeps it before serving anything.
    $offset = -s $node->logfile;
    stop_worker($node, 'postgres');
    is has_store($oid), 0, 'The restarted worker should sweep the store';
    ok $node->log_contains(qr/chdb_search: swept orphan store idx_$oid\b/, $offset),
        'Should log the sweep';
}

DROPPED_DATABASE: {
    my $dboid = make_store('d2');

    # A backend without the library drops the database: nothing tells the
    # worker of another database, so the directory stays until a worker next
    # starts. A worker that crashed off Linux, where its socket is a file,
    # would have left the socket too.
    $node->safe_psql(postgres => 'DROP DATABASE d2');
    ok -d "$pg_chdb/$dboid",
        'The directory should survive a DROP DATABASE without the library';
    open my $sock, '>', "$pg_chdb/$dboid.sock" or die $!;
    close $sock;

    my $offset = -s $node->logfile;
    stop_worker($node, 'postgres');
    has_store(0);
    ok !-e "$pg_chdb/$dboid", 'The next worker to start should remove the directory';
    ok !-e "$pg_chdb/$dboid.sock", 'The socket should be gone too';
    ok $node->log_contains(
        qr/chdb_search: sweeping store of dropped database $dboid\b/, $offset),
        'Should log the sweep';

    # With the library loaded the hook removes it at commit.
    $dboid = make_store('d3');
    $node->safe_psql(postgres => "LOAD 'chdb_search'; DROP DATABASE d3");
    ok !-e "$pg_chdb/$dboid",
        'A DROP DATABASE with the library should remove the directory';
}

done_testing;
