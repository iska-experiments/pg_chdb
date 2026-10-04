#!/usr/bin/perl

# A backend killed inside a transaction that has staged rows never runs the
# drop it registered for its staging table. The table is named by the full
# transaction id, so it is seen to belong to a transaction that is over, and
# the next VACUUM of the table sweeps it.

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
my $node = search_node('crash',
    "chdb_search.flush_threshold = '64kB'", 'restart_after_crash = on');
END { $node->stop if $node }

my $oid = $node->safe_psql(postgres => "SELECT 'docs_idx'::regclass::oid");

my $victim = $node->background_psql('postgres');
my $pid = $victim->query_safe('SELECT pg_backend_pid()');
$victim->query_safe(q{
    BEGIN;
    INSERT INTO docs (id, body)
    SELECT 1000 + i, repeat('word ', 250) FROM generate_series(1, 120) i;
});
like store_tables($node, $oid), qr/^t_\d+_tx_\d+$/m, 'The transaction should have staged its rows';

# An uncommitted transaction's WAL need not have reached disk yet. Without
# it the crash loses the transaction's xid too, which then reads as still to
# come, and the sweep keeps the table until the counter passes it; put it on
# disk so that the transaction is seen to be over.
$node->safe_psql(postgres => 'CHECKPOINT');

# Crash recovery restarts everything; the staging table is still there.
my $offset = -s $node->logfile;
is kill('KILL', $pid), 1, 'Should kill the backend';
$node->wait_for_log(qr/database system is ready to accept connections/, $offset);
eval { $victim->quit };
like store_tables($node, $oid), qr/^t_\d+_tx_\d+$/m, 'The staging table should survive the crash';
is $node->safe_psql(postgres => 'SELECT count(*) FROM docs WHERE id > 1000'), 0,
    'The rows should not';

# VACUUM finds the transaction over and sweeps the table.
$offset = -s $node->logfile;
$node->safe_psql(postgres => 'VACUUM docs');
ok $node->log_contains(
    qr/chdb_search exec: DROP TABLE IF EXISTS idx_$oid\.t_\d+_tx_\d+/, $offset),
    'VACUUM should drop the staging table';
unlike store_tables($node, $oid), qr/_tx_/, 'The staging table should be gone';
like store_tables($node, $oid), qr/^t_\d+$/m, 'The index table should stay';

done_testing;
