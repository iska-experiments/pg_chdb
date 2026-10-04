#!/usr/bin/perl

# Point-in-time recovery from a base backup and archived WAL. The heap comes
# back as it was at the recovery target, while the store is the copy the
# backup took, so an index flushed between the backup and the target is
# ahead of its store: the fail-safe refuses it as chdb_search.unavailable_index
# says, and REINDEX rebuilds it from the restored heap.

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

$primary->backup('bkp');
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
