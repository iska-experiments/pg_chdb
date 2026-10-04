#!/usr/bin/perl

# Point-in-time recovery from a base backup and archived WAL. The backup and
# the archive carry the index pages with the heap, so a server recovered to
# a target holds the rows committed up to it and the index answers for
# exactly those, with no REINDEX; recovery over, it takes new rows. The
# engine's directory is left out of the backup, as every pgsql_tmp is, and
# the worker makes it again.

use v5.34;
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

plan skip_all => 'needs the chdb_search worker' if $ENV{CHDB_SEARCH_STUB};

my $primary  = search_node('pitr_primary', { allows_streaming => 1, has_archiving => 1 });
my $restored = PostgreSQL::Test::Cluster->new('pitr_restored');
END { $_->stop for grep { $_ } ($restored, $primary) }

my $dboid = $primary->safe_psql(postgres =>
    "SELECT oid FROM pg_database WHERE datname = 'postgres'");
$primary->backup('bkp');
ok !-e $primary->backup_dir . "/bkp/pg_chdb/pgsql_tmp/$dboid",
    'The backup should leave the engine directory out';
my $target = pitr_rows($primary);

$restored->init_from_backup($primary, 'bkp', has_restoring => 1, standby => 0);
$restored->append_conf('postgresql.conf', qq{
recovery_target_name = '$target'
recovery_target_action = 'promote'
});
$restored->start;
$restored->poll_query_until(postgres => 'SELECT NOT pg_is_in_recovery()')
    or die 'recovery did not finish';
ok $restored->log_contains(qr/recovery stopping at restore point "$target"/),
    'Should recover to the restore point';
check_restored($restored);

done_testing;
