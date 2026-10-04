#!/usr/bin/perl

# pg_upgrade restores the schema with CREATE INDEX before any data is moved.
# A chdb index built then would get an empty store behind a valid index, or
# drop a store the operator had copied over by hand. In binary-upgrade mode
# ambuild writes the metapage, warns, and sends the worker nothing; REINDEX
# after the upgrade builds the store.

use v5.34;
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

my $node = search_node('upgrade');
END { $node->stop if $node }

# Cluster->start takes no postmaster options, so start pg_upgrade's way by
# hand, and tell the node its postmaster so that it can stop it.
$node->stop;
PostgreSQL::Test::Utils::system_or_bail(
    'pg_ctl', '--wait', '--pgdata' => $node->data_dir,
    '--log' => $node->logfile, '--options' => '-b', 'start');
$node->_update_pid(1);
cmp_ok $node->safe_psql(postgres => 'SHOW data_directory'), 'ne', '',
    'Should connect to the server in binary-upgrade mode';

# pg_dump's binary-upgrade output reserves the OID and relfilenode first.
my $oid    = 90000;
my $offset = -s $node->logfile;
my ($out, $err) = ('', '');
my $ret = $node->psql(postgres => qq{
    SELECT binary_upgrade_set_next_index_pg_class_oid($oid);
    SELECT binary_upgrade_set_next_index_relfilenode($oid);
    CREATE INDEX docs_upgraded ON docs USING chdb (body);
}, stdout => \$out, stderr => \$err);
is $ret, 0, 'Should create the index in binary-upgrade mode';
like $err,
    qr/WARNING:\s+chdb index "docs_upgraded" is restored by pg_upgrade without its store/,
    'Should warn that the store is missing';
like $err, qr/HINT:\s+Run REINDEX INDEX after the upgrade\./,
    'Should name REINDEX';
ok !$node->log_contains(qr/chdb_search (?:exec|insert): /, $offset),
    'Should send the worker nothing';
is $node->safe_psql(postgres => q{
    SELECT indisvalid::text || ' ' || pg_relation_size('docs_upgraded')
      FROM pg_index WHERE indexrelid = 'docs_upgraded'::regclass
}), 'true 8192', 'Should leave a valid index with just its metapage';
is $node->safe_psql(postgres => "SELECT 'docs_upgraded'::regclass::oid"), $oid,
    'Should use the reserved OID';

# After the upgrade REINDEX builds the store the index was restored without.
$node->stop;
$node->start;
$offset = -s $node->logfile;
$node->safe_psql(postgres => 'REINDEX INDEX docs_upgraded');
ok $node->log_contains(qr/chdb_search exec: CREATE TABLE idx_$oid\.t_\d+ /, $offset),
    'REINDEX should build the store';
ok $node->log_contains(
    qr/chdb_search insert: INSERT INTO idx_$oid\.t_\d+ .* -- 2 rows/, $offset),
    'REINDEX should load the heap into it';

done_testing;
