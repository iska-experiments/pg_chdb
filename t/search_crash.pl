#!/usr/bin/perl

# Crashes with rows in flight. A backend killed inside a transaction that
# has staged rows never runs the drop of its staging table; the table is
# named by the full transaction id, so it is seen to belong to a
# transaction that is over, and the next VACUUM of the table sweeps it.
# While the transaction runs, its own searches read the staging table and
# no other session's do. A postmaster killed in the middle of a run of
# commits loses the engine and the worker with it; crash recovery replays
# the index pages with the heap, and the index answers for exactly the
# rows the heap kept, no REINDEX.

use v5.34;
use strict;
use warnings FATAL => 'all';
use IPC::Run;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

plan skip_all => 'needs the chdb_search worker' if $ENV{CHDB_SEARCH_STUB};

# The node's default is to shut down on a crash; let it recover instead.
my $node = search_node('crash',
    "chdb_search.flush_threshold = '64kB'", 'restart_after_crash = on');
END { $node->stop if $node }

my $oid = $node->safe_psql(postgres => "SELECT 'docs_idx'::regclass::oid");

my $victim = $node->background_psql('postgres');
my $pid = $victim->query_safe('SELECT pg_backend_pid()');
$victim->query_safe(q{
    BEGIN;
    INSERT INTO docs (id, body)
    SELECT 1000 + i, 'staged ' || repeat('word ', 250) FROM generate_series(1, 120) i;
});
like store_tables($node, $oid), qr/^t_\d+_tx_\d+$/m, 'The transaction should have staged its rows';

# Its own search ships the rest and reads the staging table with the index's;
# another session's search reads the index's table alone.
my $search = q{SET enable_seqscan = off; SELECT count(*) FROM docs WHERE body @@@ 'staged'};
is $victim->query_safe($search), 120, 'The transaction should see its own rows';
is $node->safe_psql(postgres => $search), 0,
    'Another session should not see them';

# An uncommitted transaction's WAL need not have reached disk yet. Without
# it the crash loses the transaction's xid too, which then reads as still to
# come, and the sweep keeps the table until the counter passes it; put it on
# disk so that the transaction is seen to be over.
$node->safe_psql(postgres => 'CHECKPOINT');

# Crash recovery restarts everything, the worker among it, which starts its
# engine without tables and attaches the index's, the staging table among
# them, when the first request names the index.
my $offset = -s $node->logfile;
is kill('KILL', $pid), 1, 'Should kill the backend';
$node->wait_for_log(qr/database system is ready to accept connections/, $offset);
eval { $victim->quit };
is $node->safe_psql(postgres =>
    "SET enable_seqscan = off; SELECT id FROM docs WHERE body @@@ 'boots'"), 2,
    'The index should answer after the crash';
like store_tables($node, $oid), qr/^t_\d+_tx_\d+$/m, 'The staging table should survive the crash';
is $node->safe_psql(postgres => 'SELECT count(*) FROM docs WHERE id > 1000'), 0,
    'The rows should not';

# VACUUM finds the transaction over and sweeps the table.
$offset = -s $node->logfile;
$node->safe_psql(postgres => 'VACUUM docs');
ok $node->log_contains(
    qr/chdb_search exec: DROP TABLE IF EXISTS idx_$oid\.t_\d+_tx_\d+/, $offset),
    'VACUUM should drop the staging table';
unlike store_tables($node, $oid), qr/_tx_/, 'The staging table should be gone';
like store_tables($node, $oid), qr/^t_\d+$/m, 'The index table should stay';

# A run of single-row commits, each a flush into the store, with the
# postmaster killed in the middle of it: the heap keeps the rows whose
# commit records reached the WAL, and the index's pages, written ahead of
# those records, keep their parts; whatever was in flight is lost on both
# sides alike.
my $dir = PostgreSQL::Test::Utils::tempdir;
my $file = "$dir/commits.sql";
open my $fh, '>', $file or die "$file: $!";
print $fh "INSERT INTO docs VALUES ($_, 'crash boots $_');\n" for 2001 .. 2600;
close $fh;
my ($out, $err) = ('', '');
my $commits = IPC::Run::start(
    ['psql', '-X', '-qtA', '-d', $node->connstr('postgres'), '-f', $file],
    \undef, \$out, \$err);
$node->poll_query_until(postgres => 'SELECT count(*) > 40 FROM docs WHERE id > 2000')
    or die 'the commits did not start';
$node->kill9;
$commits->finish;
$node->start;
my $ids = q{string_agg(id::text, ',' ORDER BY id)};
my $heap = $node->safe_psql(postgres => "SELECT $ids FROM docs WHERE id > 2000");
cmp_ok scalar(split /,/, $heap), '>', 40, 'The heap should have kept the committed rows';
is $node->safe_psql(postgres =>
    "SET enable_seqscan = off; SELECT $ids FROM docs WHERE body @@@ 'crash'"),
    $heap, 'The index should answer with exactly the rows the heap kept';
$node->safe_psql(postgres => "INSERT INTO docs VALUES (3000, 'crash boots after')");
is $node->safe_psql(postgres =>
    "SET enable_seqscan = off; SELECT $ids FROM docs WHERE body @@@ 'crash'"),
    "$heap,3000", 'The index should take new rows after the crash';
ok !$node->log_contains(qr/not available on this server|does not match its store/, 0),
    'Nothing should have asked for a REINDEX';

done_testing;
