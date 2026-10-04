#!/usr/bin/perl

# Two-phase commit with prepared transactions enabled: a store drop or build
# cannot be carried past PREPARE and is refused by the access method, not by
# the setting, while transactions it has nothing to carry for prepare and
# commit as before.

use v5.34;
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

my $node = search_node('prepare',
    'max_prepared_transactions = 2',
    "session_preload_libraries = 'chdb_search'");
END { $node->stop if $node }

my $oid = $node->safe_psql(postgres => "SELECT 'docs_idx'::regclass::oid");
my $offset = -s $node->logfile;

# A read-only transaction, with the library and its callback loaded.
$node->safe_psql(postgres => q{
    BEGIN;
    SELECT count(*) FROM docs;
    PREPARE TRANSACTION 'ro';
    COMMIT PREPARED 'ro';
});
is $node->safe_psql(postgres => 'SELECT count(*) FROM pg_prepared_xacts'), 0,
    'Should prepare and commit a read-only transaction';

# A store drop is refused before the transaction is marked as preparing.
my ($out, $err) = ('', '');
$node->psql(postgres => "BEGIN; DROP INDEX docs_idx; PREPARE TRANSACTION 'p';",
    stdout => \$out, stderr => \$err);
like $err, qr/cannot PREPARE a transaction that created or dropped a chdb index/,
    'Should refuse to prepare a DROP INDEX';
is $node->safe_psql(postgres => 'SELECT count(*) FROM pg_prepared_xacts'), 0,
    'Should leave nothing prepared';
is $node->safe_psql(postgres => "SELECT count(*) FROM pg_class WHERE oid = $oid"), 1,
    'Should keep the index';
ok !$node->log_contains(qr/chdb_search drop: idx_$oid\b/, $offset),
    'Should keep its store';

# So is a build, and the store it had made goes with the transaction.
$offset = -s $node->logfile;
($out, $err) = ('', '');
$node->psql(postgres => q{
    BEGIN;
    CREATE INDEX docs_prep ON docs USING chdb (body);
    PREPARE TRANSACTION 'q';
}, stdout => \$out, stderr => \$err);
like $err, qr/cannot PREPARE a transaction that created or dropped a chdb index/,
    'Should refuse to prepare a CREATE INDEX';
ok $node->log_contains(qr/chdb_search drop: idx_\d+/, $offset),
    'Should drop the new store with the transaction';

# Buffered rows are refused by the buffer.
($out, $err) = ('', '');
$node->psql(postgres => q{
    BEGIN;
    INSERT INTO docs VALUES (3, 'prepared shoes');
    PREPARE TRANSACTION 'r';
}, stdout => \$out, stderr => \$err);
like $err, qr/cannot PREPARE a transaction that changed a chdb index/,
    'Should refuse to prepare buffered rows';

# A rebuild whose PREPARE fails after the access method let it through drops
# the generation it wrote with the transaction. search_am_rebuild meets this
# with prepared transactions disabled, where the line races the error to the
# client; a GID in use fails MarkAsPreparing the same way.
$node->safe_psql(postgres => "BEGIN; SELECT 1; PREPARE TRANSACTION 'held';");
$offset = -s $node->logfile;
($out, $err) = ('', '');
$node->psql(postgres => q{
    BEGIN;
    REINDEX INDEX docs_idx;
    PREPARE TRANSACTION 'held';
}, stdout => \$out, stderr => \$err);
like $err, qr/transaction identifier "held" is already in use/,
    'Should fail to prepare a REINDEX under a GID in use';
ok $node->log_contains(qr/chdb_search exec: DROP TABLE IF EXISTS idx_$oid\.t_\d+/, $offset),
    'Should drop the generation the failed REINDEX wrote';
$node->safe_psql(postgres => "COMMIT PREPARED 'held'");

# A rebuild may be prepared: its abort would only have dropped a table VACUUM
# sweeps. Committed, the index serves from the new generation.
$node->safe_psql(postgres => q{
    BEGIN;
    REINDEX INDEX docs_idx;
    PREPARE TRANSACTION 's';
    COMMIT PREPARED 's';
});
is $node->safe_psql(postgres => 'SELECT count(*) FROM pg_prepared_xacts'), 0,
    'Should prepare and commit a REINDEX';
my $found = $node->safe_psql(postgres => q{
    SET enable_seqscan = off;
    SELECT id FROM docs WHERE body @@@ 'boots';
});
SKIP: {
    skip 'the stub client returns no rows', 1 if $ENV{CHDB_SEARCH_STUB};
    is $found, 2, 'Should search the rebuilt store';
}

done_testing;
