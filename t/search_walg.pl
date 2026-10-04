#!/usr/bin/perl

# WAL-G backing up and restoring a cluster with a chdb index whose store is
# a local directory: backup-push of the data directory, wal-push as the
# archive_command, backup-fetch and wal-fetch as the restore_command to a
# recovery target. The store comes back as the backup copied it, a flush
# behind the restored index, and the fail-safe refuses it until REINDEX, as
# after any restore. Skipped without a wal-g binary on the PATH or in WALG.
#
# WAL-G tars every file under the data directory and tar has no entry for a
# socket, so a worker listening on pg_chdb/<dboid>.sock failed backup-push.
# On Linux the worker listens in the abstract namespace instead, so the
# backup runs with the worker up; and the restored cluster, on the same host
# with the same database OID, listens on a name of its own.

use v5.34;
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

plan skip_all => 'needs the chdb_search worker' if $ENV{CHDB_SEARCH_STUB};
my ($walg) = grep { -x } ($ENV{WALG} // (), map { "$_/wal-g" } split /:/, $ENV{PATH});
plan skip_all => 'needs wal-g on the PATH or in WALG' unless $walg;

# WAL-G's storage: a local directory, named in the environment of every
# wal-g the test runs and in the commands the servers run.
my $storage = PostgreSQL::Test::Utils::tempdir('walg');
local $ENV{WALG_FILE_PREFIX} = $storage;
local $ENV{PGDATABASE}       = 'postgres';
my $walg_env = "WALG_FILE_PREFIX=$storage $walg";

my $primary = search_node('walg_primary', { allows_streaming => 1 },
    'archive_mode = on', "archive_command = '$walg_env wal-push %p'");
my $restored = PostgreSQL::Test::Cluster->new('walg_restored');
END { $_->stop for grep { $_ } ($restored, $primary) }

my $pgdata = $primary->data_dir;
my $dboid  = $primary->safe_psql(postgres =>
    "SELECT oid FROM pg_database WHERE datname = 'postgres'");
my $socket = worker_socket($primary, 'postgres');
like $socket, qr{^\@pg_chdb/[0-9a-f]{16}/$dboid$},
    'The worker should listen on an abstract socket';
ok !-e "$pgdata/pg_chdb/$dboid.sock", 'The data directory should hold no socket';
$primary->command_ok([ $walg, 'backup-push', $pgdata ],
    'backup-push should back up the data directory with the worker running');
is worker_socket($primary, 'postgres'), $socket, 'The worker should still listen';

my $target = pitr_rows($primary);

# Restored where init_from_backup would put it, and configured as that does.
my $dest = $restored->data_dir;
my $host = $restored->host;
$restored->command_ok([ $walg, 'backup-fetch', $dest, 'LATEST' ],
    'backup-fetch should restore the data directory');
chmod 0700, $dest or die "chmod $dest: $!";
$restored->append_conf('postgresql.conf', join "\n",
    'port = ' . $restored->port,
    $PostgreSQL::Test::Utils::use_unix_sockets
        ? "unix_socket_directories = '$host'" : "listen_addresses = '$host'",
    "restore_command = '$walg_env wal-fetch %f %p'",
    "recovery_target_name = '$target'",
    "recovery_target_action = 'promote'");
$restored->set_recovery_mode;
$restored->start;
$restored->poll_query_until(postgres => 'SELECT NOT pg_is_in_recovery()')
    or die 'recovery did not finish';
ok $restored->log_contains(qr/recovery stopping at restore point "$target"/),
    'Should recover to the restore point';
check_restored($restored);
ok worker_socket($restored, 'postgres') ne $socket,
    'The restored cluster\'s worker should listen on a name of its own';

done_testing;
