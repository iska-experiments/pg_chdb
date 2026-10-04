#!/usr/bin/perl

# A streaming standby made from a base backup of the primary. Phase 0 keeps
# the store in a directory under the data directory that WAL knows nothing
# of, so the standby has the copy the backup took and starts no worker to
# serve it: a search through the index is refused as
# chdb_search.unavailable_index says, an error by default or a plan without
# the index. Promoted, the standby is a primary whose store is a flush
# behind the metapage it replayed, which the same check refuses until
# REINDEX rebuilds the store from the heap.

use v5.34;
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

plan skip_all => 'needs the chdb_search worker' if $ENV{CHDB_SEARCH_STUB};

my $primary = search_node('primary', { allows_streaming => 1 });
my $standby = PostgreSQL::Test::Cluster->new('standby');
END { $_->stop for grep { $_ } ($standby, $primary) }

# pg_basebackup copies the store directory and warns of the worker's socket,
# a special file it skips.
$primary->backup('bkp');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->start;

# A row committed after the backup: the standby replays the flush into the
# metapage, while its copy of the store is from before it.
$primary->safe_psql(postgres => "INSERT INTO docs VALUES (3, 'Hiking boots')");
$primary->wait_for_catchup($standby);
is $standby->safe_psql(postgres => 'SELECT id FROM docs ORDER BY id'), "1\n2\n3",
    'The standby should have the row';

# In recovery the store is not asked, so no worker starts.
check_unavailable($standby, "2\n3",
    qr/Its store is not current on a server in recovery\./,
    qr/REINDEX the index once the server is out of recovery\./);
my $dboid = $standby->safe_psql(postgres =>
    "SELECT oid FROM pg_database WHERE datname = 'postgres'");
ok -d $standby->data_dir . "/pg_chdb/$dboid", 'The backup should have copied the store';
ok !-e $standby->data_dir . "/pg_chdb/$dboid.sock", 'The standby should have no socket';
is $standby->safe_psql(postgres =>
    "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'chdb_search worker'"),
    0, 'The standby should start no worker';

# Promoted, the server asks the store, which is a flush behind the metapage.
$standby->promote;
$standby->poll_query_until(postgres => 'SELECT NOT pg_is_in_recovery()')
    or die 'promotion did not finish';
check_unavailable($standby, "2\n3",
    qr/The store was last flushed at [0-9A-F]+\/[0-9A-F]+, the index at/,
    qr/REINDEX INDEX "docs_idx" rebuilds its store\./);
ok worker_pid($standby, 'postgres'), 'The promoted server should have a worker';

# REINDEX rebuilds the store from the heap, and the index serves and grows.
$standby->safe_psql(postgres => 'REINDEX INDEX docs_idx');
is search_ids($standby, 'boots'), "2\n3", 'REINDEX should make the index available';
$standby->safe_psql(postgres => "INSERT INTO docs VALUES (4, 'Climbing boots')");
is search_ids($standby, 'boots'), "2\n3\n4", 'The promoted server should index new rows';
is search_ids($primary, 'boots'), "2\n3", 'The old primary should answer as before';

done_testing;
