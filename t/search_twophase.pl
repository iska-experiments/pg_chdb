#!/usr/bin/perl

# Two-phase commit through the store. PREPARE TRANSACTION flushes the
# transaction's buffered and staged rows as COMMIT does, since COMMIT
# PREPARED and ROLLBACK PREPARED run no callback of the access method's. The
# rows are in the store from PREPARE on, and the heap decides what a search
# returns: nothing of a transaction still prepared, nothing of one rolled
# back, whose rows VACUUM removes from the store, everything of one
# committed.

use v5.34;
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

plan skip_all => 'needs the chdb_search worker' if $ENV{CHDB_SEARCH_STUB};

my $node = search_node('twophase', 'max_prepared_transactions = 2');
END { $node->stop if $node }

my $oid = $node->safe_psql(postgres => "SELECT 'docs_idx'::regclass::oid");

# A prepared row is in the store and hidden by the heap.
my $offset = -s $node->logfile;
$node->safe_psql(postgres => q{
    BEGIN;
    INSERT INTO docs VALUES (3, 'Hiking boots');
    PREPARE TRANSACTION 'ins';
});
ok $node->log_contains(
    qr/chdb_search insert: INSERT INTO idx_$oid\.t_\d+ .* -- 1 rows/, $offset),
    'PREPARE should flush the buffered row';
ok $node->log_contains(qr/chdb_search exec: INSERT INTO idx_$oid\.meta /, $offset),
    'PREPARE should record the flush in the store';
is store_rows($node), 3, 'The store should hold the prepared row';
is search_ids($node, 'boots'), 2, 'A search should not return the prepared row';

# Rolled back, the row is dead in the heap and stays hidden until VACUUM
# removes it from the store.
$node->safe_psql(postgres => "ROLLBACK PREPARED 'ins'");
is search_ids($node, 'boots'), 2, 'A search should not return the rolled-back row';
is store_rows($node), 3, 'The store should still hold it';
$offset = -s $node->logfile;
$node->safe_psql(postgres => 'VACUUM docs');
ok $node->log_contains(
    qr/chdb_search exec: DELETE FROM idx_$oid\.t_\d+ WHERE ctid IN \(\d+\)/, $offset),
    'VACUUM should delete the dead row from the store';
is store_rows($node), 2, 'The store should be back to the live rows';

# Committed, the row is searchable.
$node->safe_psql(postgres => q{
    BEGIN;
    INSERT INTO docs VALUES (4, 'Climbing boots');
    PREPARE TRANSACTION 'ins';
});
is search_ids($node, 'boots'), 2, 'A search should not return a prepared row';
$node->safe_psql(postgres => "COMMIT PREPARED 'ins'");
is search_ids($node, 'boots'), "2\n4", 'COMMIT PREPARED should make the row searchable';

# A transaction past flush_threshold has staged rows in a table of its own,
# which PREPARE merges into the index's table and drops, as COMMIT would.
my $staged = q{SET enable_seqscan = off; SELECT count(*) FROM docs WHERE body @@@ 'staged'};
$offset = -s $node->logfile;
$node->safe_psql(postgres => q{
    BEGIN;
    SET LOCAL chdb_search.flush_threshold = '64kB';
    INSERT INTO docs SELECT 100 + i, 'staged boots ' || i FROM generate_series(1, 5000) i;
    PREPARE TRANSACTION 'staged';
});
ok $node->log_contains(
    qr/chdb_search exec: INSERT INTO idx_$oid\.t_\d+ SELECT \* FROM idx_$oid\.t_\d+_tx_\d+/,
    $offset), 'PREPARE should merge the staging table';
unlike store_tables($node, $oid), qr/_tx_/, 'PREPARE should drop the staging table';
is $node->safe_psql(postgres => $staged), 0, 'The staged rows should wait for the commit';
$node->safe_psql(postgres => "COMMIT PREPARED 'staged'");
is $node->safe_psql(postgres => $staged), 5000, 'COMMIT PREPARED should make them searchable';

done_testing;
