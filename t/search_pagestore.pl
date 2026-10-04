#!/usr/bin/perl

# The blob store behind a chdb index: the engine keeps its store tables on
# libchdb's callback object storage and asks the worker for every blob, which
# this phase keeps as files under pg_chdb/<dboid>/blobs/pg_<indexoid>/. A
# write is pending in a temporary file until the engine commits it, so an
# engine killed in the middle of writing leaves pending files for the worker
# to drop, and nothing else: the worker survives, the request in flight
# fails naming the signal, and the next engine finds the committed blobs. A
# new worker finds them too, registering the storages it holds before its
# engine opens the store.

use v5.34;
use strict;
use warnings FATAL => 'all';
use IPC::Run qw(start);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

plan skip_all => 'needs the chdb_search worker' if $ENV{CHDB_SEARCH_STUB};

# Small flushes: a long insert stages its rows in many parts, each written
# through the worker blob by blob, so a kill lands inside one.
my $node = search_node('pagestore', "chdb_search.flush_threshold = '64kB'");
END { $node->stop if $node }

my $dboid = $node->safe_psql(postgres =>
    "SELECT oid FROM pg_database WHERE datname = 'postgres'");
my $oid     = $node->safe_psql(postgres => "SELECT 'docs_idx'::regclass::oid");
my $blobs   = $node->data_dir . "/pg_chdb/$dboid/blobs";
my $pending = "$blobs/.tmp";

sub search {
    return $node->safe_psql(postgres =>
        "SET enable_seqscan = off; SELECT id FROM docs WHERE body @@@ 'boots' ORDER BY id");
}

# The files under a directory, none when it does not exist.
sub files_in {
    my $dir = shift;
    opendir my $dh, $dir or return ();
    return grep { !/^\./ } readdir $dh;
}

sub blob_count {
    return $node->safe_psql(postgres => "SELECT count(*) FROM chdb_search_blobs('docs_idx')");
}

is search(), 2, 'The index should answer a search';
ok -d "$blobs/pg_$oid", 'The index should have a blob storage';
cmp_ok blob_count(), '>', 0, 'The storage should hold the parts of the index';
ok -d $pending, 'Pending writes should have a directory';
is scalar(files_in($pending)), 0, 'Nothing should be pending between requests';

my $worker = worker_pid($node, 'postgres');
my $engine = $node->safe_psql(postgres => 'SELECT chdb_search_engine_pid()');
ok $engine, 'Should have an engine';

# A long insert, in the background: its stages flow through the worker to
# the engine, which writes a part for each.
my ($out, $err) = ('', '');
my $insert = start [
    'psql', '-X', '-qtA', '-v', 'ON_ERROR_STOP=1', '-d', $node->connstr('postgres'),
    '-c', q{INSERT INTO docs SELECT 1000 + i, 'walking boots ' || repeat('word ', 200) || i
             FROM generate_series(1, 200000) i}
], \undef, \$out, \$err;

# The moment a blob is pending, kill the engine mid-write.
my $seen = 0;
for (1 .. 3000) {
    $insert->pump_nb;
    if (files_in($pending)) {
        $seen = 1;
        last;
    }
    select undef, undef, undef, 0.01;
}
ok $seen, 'Should see a pending blob write';
my $offset = -s $node->logfile;
is kill('KILL', $engine), 1, 'Should kill the engine mid-write';
$insert->finish;
like $err, qr/chDB engine \(pid $engine\) was terminated by signal 9\b/,
    'The insert should fail naming the signal';

# The worker goes on, having dropped what the dead engine left half written.
is worker_pid($node, 'postgres'), $worker, 'The worker should survive';
ok $node->log_contains(qr/chdb_search: dropped \d+ pending blob writes of the chDB engine/, $offset),
    'The worker should log the pending writes it dropped';
is scalar(files_in($pending)), 0, 'No pending write should be left behind';
is search(), 2, 'The next engine should serve the committed blobs';
is $node->safe_psql(postgres => 'SELECT count(*) FROM docs'), 2,
    'The failed insert should have committed nothing';
cmp_ok $node->safe_psql(postgres => 'SELECT chdb_search_engine_pid()'), 'ne', $engine,
    'Should be served by a new engine';
unlike slurp_file($node->logfile, $offset), qr/terminating any other active server processes/,
    'The instance should not restart';

# A new worker registers the storages it finds before its engine opens the
# store, so the tables persisted on them attach.
$node->safe_psql(postgres => q{
    SELECT pg_terminate_backend(pid) FROM pg_stat_activity
     WHERE backend_type = 'chdb_search worker' AND datname = 'postgres'
});
$node->poll_query_until(postgres => q{
    SELECT count(*) = 0 FROM pg_stat_activity
     WHERE backend_type = 'chdb_search worker' AND datname = 'postgres'
}) or die 'the worker did not stop';
$node->safe_psql(postgres => "INSERT INTO docs VALUES (3, 'Hiking boots')");
is search(), "2\n3", 'A new worker should find the storage and serve it';
cmp_ok worker_pid($node, 'postgres'), 'ne', $worker, 'Should be a new worker';

# DROP INDEX drops the store tables, whose blobs go with them, storage and
# all. (The library is loaded first: a session loading it inside the DROP
# would miss the drop, as t/search_sweep.pl pins.)
$node->safe_psql(postgres => "LOAD 'chdb_search'; DROP INDEX docs_idx");
ok !-e "$blobs/pg_$oid", 'The dropped index should leave no blobs';

done_testing;
