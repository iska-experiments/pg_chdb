#!/usr/bin/perl

# The blob store behind a chdb index: the engine keeps its store tables on
# libchdb's callback object storage and asks the worker for every blob,
# which lives in the pages of the index relation. A write is pending, its
# pages taken but named by no directory entry, until the engine commits it,
# so an engine killed in the middle of writing leaves pending writes for the
# worker to drop and nothing else: the worker survives, the request in
# flight fails naming the signal, and the next engine finds the committed
# blobs. A new worker finds them too, and DROP INDEX takes them with the
# relation.

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

my $relfile = $node->data_dir . '/' .
    $node->safe_psql(postgres => "SELECT pg_relation_filepath('docs_idx')");

sub search {
    return $node->safe_psql(postgres =>
        "SET enable_seqscan = off; SELECT id FROM docs WHERE body @@@ 'boots' ORDER BY id");
}

sub blob_count {
    return $node->safe_psql(postgres => "SELECT count(*) FROM chdb_search_blobs('docs_idx')");
}

sub pages {
    return $node->safe_psql(postgres => "SELECT pg_relation_size('docs_idx') / 8192");
}

is search(), 2, 'The index should answer a search';
ok -e $relfile, 'The index should have a relation file';
cmp_ok blob_count(), '>', 0, 'The pages should hold the parts of the index';
cmp_ok pages(), '>', 1, 'The relation should have pages beyond the metapage';

my $worker = worker_pid($node, 'postgres');
my $engine = $node->safe_psql(postgres => 'SELECT chdb_search_engine_pid()');
ok $engine, 'Should have an engine';

# A long insert in the background: its stages flow through the worker to
# the engine, which writes a part for each, blob by blob, holding every
# file of the part open until it is done. The stages are large, so the
# engine is inside a part nearly all the time; the kill lands there, or the
# insert is tried again. The engine's pid is taken before the insert
# starts: asked during it, the worker answers between two parts, where a
# kill finds nothing pending.
my $offset = -s $node->logfile;
my ($out, $err, $tries) = ('', '', 0);
until ($node->log_contains(qr/chdb_search: dropped \d+ pending blob writes of the chDB engine/, $offset)
       || $tries++ == 5) {
    my $before = blob_count();
    $engine = $node->safe_psql(postgres => 'SELECT chdb_search_engine_pid()');
    ($out, $err) = ('', '');
    my $insert = start [
        'psql', '-X', '-qtA', '-v', 'ON_ERROR_STOP=1', '-d', $node->connstr('postgres'),
        '-c', q{SET chdb_search.flush_threshold = '1MB';
                INSERT INTO docs SELECT 1000 + i, 'walking boots ' || repeat('word ', 200) || i
                  FROM generate_series(1, 200000) i}
    ], \undef, \$out, \$err;
    for (1 .. 3000) {
        $insert->pump_nb;
        last if blob_count() > $before + 30;
        select undef, undef, undef, 0.01;
    }
    is kill('KILL', $engine), 1, "Should kill the engine mid-insert (try $tries)";
    $insert->finish;
    like $err, qr/chDB engine \(pid $engine\) was terminated by signal 9\b/,
        'The insert should fail naming the signal';
}

# The worker goes on, having dropped what the dead engine left half written.
is worker_pid($node, 'postgres'), $worker, 'The worker should survive';
ok $node->log_contains(qr/chdb_search: dropped \d+ pending blob writes of the chDB engine/, $offset),
    'The worker should log the pending writes it dropped';
is search(), 2, 'The next engine should serve the committed blobs';
is $node->safe_psql(postgres => 'SELECT count(*) FROM docs'), 2,
    'The failed insert should have committed nothing';
cmp_ok $node->safe_psql(postgres => 'SELECT chdb_search_engine_pid()'), 'ne', $engine,
    'Should be served by a new engine';
unlike slurp_file($node->logfile, $offset), qr/terminating any other active server processes/,
    'The instance should not restart';

# Pages come back: the parts of a staging table go to the free stack when a
# rollback drops the table, and the next parts take them rather than growing
# the relation. The rows are hard to compress, so each part has data pages
# beside its directory entries. (A merge frees its parts the same way, once
# ClickHouse drops them, old_parts_lifetime after the merge.)
$node->safe_psql(postgres => 'VACUUM docs');
my $rows = q{INSERT INTO docs SELECT %d + i, 'walking boots ' || repeat(md5(i::text), 32)
             FROM generate_series(1, %d) i};
$node->safe_psql(postgres => 'BEGIN; ' . sprintf($rows, 2000, 3000) . '; ROLLBACK');
my $freed = pages();
$node->safe_psql(postgres => sprintf($rows, 5000 + 100 * $_, 100)) for 1 .. 20;
cmp_ok pages(), '<=', $freed + 4, 'New parts should reuse the pages the rollback freed';
is $node->safe_psql(postgres => "SET enable_seqscan = off; SELECT count(*) FROM docs WHERE body @@@ 'boots'"),
    2001, 'The new rows should be found';

# A new worker learns the relations from the requests it serves, and the
# engine attaches the tables again from the catalog.
$node->safe_psql(postgres => q{
    SELECT pg_terminate_backend(pid) FROM pg_stat_activity
     WHERE backend_type = 'chdb_search worker' AND datname = 'postgres'
});
$node->poll_query_until(postgres => q{
    SELECT count(*) = 0 FROM pg_stat_activity
     WHERE backend_type = 'chdb_search worker' AND datname = 'postgres'
}) or die 'the worker did not stop';
$node->safe_psql(postgres => "INSERT INTO docs VALUES (3, 'Hiking boots')");
is $node->safe_psql(postgres => "SET enable_seqscan = off; SELECT count(*) FROM docs WHERE body @@@ 'boots'"),
    2002, 'A new worker should find the pages and serve them';
cmp_ok worker_pid($node, 'postgres'), 'ne', $worker, 'Should be a new worker';

# DROP INDEX unlinks the relation, and the blobs with it; the engine's
# removal of each blob finds nothing to do. (The library is loaded first: a
# session loading it inside the DROP would miss the drop, as
# t/search_sweep.pl pins.)
$node->safe_psql(postgres => "LOAD 'chdb_search'; DROP INDEX docs_idx; CHECKPOINT");
ok !-e $relfile, 'The dropped index should leave no relation file';

done_testing;
