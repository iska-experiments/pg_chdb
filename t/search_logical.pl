#!/usr/bin/perl

# Logical replication: the subscriber applies rows to a table of its own,
# whose chdb index is maintained through aminsert like any other, so the
# commits of the table sync and apply workers flush to the subscriber's
# store, and the rows are searchable there once they have committed. The
# publisher's store is not involved.

use v5.34;
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

plan skip_all => 'needs the chdb_search worker' if $ENV{CHDB_SEARCH_STUB};

my $publisher  = search_node('publisher', { allows_streaming => 'logical' });
my $subscriber = search_node('subscriber');
END { $_->stop for grep { $_ } ($subscriber, $publisher) }

# The subscriber's docs starts empty, or the initial copy would duplicate the
# publisher's rows; TRUNCATE rebuilds its index with an empty store.
$subscriber->safe_psql(postgres => 'TRUNCATE docs');
is search_ids($subscriber, 'boots'), '', 'The subscriber should start with an empty index';

my $connstr = $publisher->connstr . ' dbname=postgres';
my $offset  = -s $subscriber->logfile;
$publisher->safe_psql(postgres => 'CREATE PUBLICATION pub FOR TABLE docs');
$subscriber->safe_psql(postgres =>
    "CREATE SUBSCRIPTION sub CONNECTION '$connstr' PUBLICATION pub");
$subscriber->wait_for_subscription_sync($publisher, 'sub');
is search_ids($subscriber, 'boots'), 2,
    'The initial copy should be searchable through the subscriber\'s index';
ok $subscriber->log_contains(
    qr/chdb_search insert: INSERT INTO idx_\d+\.t_\d+ .* -- 2 rows/, $offset),
    'The table sync should have flushed the copied rows at its commit';

# Rows applied later are searchable once the apply worker commits.
$offset = -s $subscriber->logfile;
$publisher->safe_psql(postgres => "INSERT INTO docs VALUES (3, 'Hiking boots')");
$publisher->wait_for_catchup('sub');
is search_ids($subscriber, 'boots'), "2\n3", 'An applied insert should be searchable';
ok $subscriber->log_contains(
    qr/chdb_search insert: INSERT INTO idx_\d+\.t_\d+ .* -- 1 rows/, $offset),
    'The apply worker should have flushed the row at its commit';

# An update indexes the new version; the heap hides the old one and the
# deleted row, as on the publisher.
$publisher->safe_psql(postgres => q{
    UPDATE docs SET body = 'Hiking sandals' WHERE id = 3;
    DELETE FROM docs WHERE id = 2;
});
$publisher->wait_for_catchup('sub');
is search_ids($subscriber, 'boots'), '', 'Updated and deleted rows should leave the search';
is search_ids($subscriber, 'sandals'), 3, 'The new version should be found';
is search_ids($publisher, 'sandals'), 3, 'The publisher should answer from its own store';

# The subscriber's store is its own, kept by its own worker, and its VACUUM
# removes the versions its heap no longer has.
ok worker_pid($subscriber, 'postgres'), 'The subscriber should have a worker';
is store_rows($subscriber), 4, 'The store should hold every version applied';
$subscriber->safe_psql(postgres => 'VACUUM docs');
is store_rows($subscriber), 2, 'VACUUM should remove the dead versions';

done_testing;
