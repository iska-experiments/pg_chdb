#!/usr/bin/perl

# The store lives in the index relation's pages, so it travels with the
# relation: a streaming standby has it and serves searches from it through
# a worker of its own, whose engine writes nothing and attaches none of the
# primary's staging tables; rows the primary commits reach the standby's
# index with its heap; a promoted standby keeps serving
# with no REINDEX, now taking rows; and the engine's directory under pg_chdb
# is a cache a worker rebuilds from the catalog and the pages when it starts.

use v5.34;
use strict;
use warnings FATAL => 'all';
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

plan skip_all => 'needs the chdb_search worker' if $ENV{CHDB_SEARCH_STUB};

my $primary = search_node('primary', { allows_streaming => 1 });
END { $primary->stop if $primary }

my $dboid = $primary->safe_psql(postgres =>
    "SELECT oid FROM pg_database WHERE datname = 'postgres'");
my $cache = "/pg_chdb/pgsql_tmp/$dboid";
my $query = "SET enable_seqscan = off; SELECT id FROM docs WHERE body @@@ 'boots' ORDER BY id";
my $plan  = "SET enable_seqscan = off; EXPLAIN (COSTS OFF) SELECT id FROM docs WHERE body @@@ 'boots'";

# A search through the index on a node: its rows, or psql's stderr.
sub search {
    my $node = shift;
    my ($out, $err) = ('', '');
    $node->psql(postgres => $query, stdout => \$out, stderr => \$err);
    return $err || $out;
}

is search($primary), 2, 'The primary should answer a search';

# A standby from a base backup, streaming.
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;
END { $standby->stop if $standby }

# Rows committed after the backup reach the standby's index pages, and a
# worker on the standby serves them, its engine reading and never writing.
$primary->safe_psql(postgres => "INSERT INTO docs VALUES (3, 'Hiking boots')");
is search($primary), "2\n3", 'The primary should answer with the new row';
$primary->wait_for_catchup($standby);
is search($standby), "2\n3", 'The standby should answer the same search from the replicated pages';
like $standby->safe_psql(postgres => $plan),
    qr/Custom Scan \(chdb_search\) on docs|Index Scan using docs_idx/,
    'The standby should plan through the index';
ok $standby->log_contains(qr/serves its indexes read-only while the server is in recovery/),
    'The standby worker should say it serves read-only';
ok $standby->log_contains(
    qr/chdb_search attach: ATTACH TABLE IF NOT EXISTS idx_\d+\.t_\d+ UUID .*, table_readonly = 1/),
    'The standby should attach the table read-only';
ok -d $standby->data_dir . $cache, 'The standby should have an engine directory of its own';

# More rows on the primary: once replayed, the standby finds the pages moved
# under its engine's table and puts the table back, so the new parts show.
$primary->safe_psql(postgres => "INSERT INTO docs VALUES (4, 'Climbing boots')");
$primary->wait_for_catchup($standby);
is search($standby), "2\n3\n4", 'The standby should see rows replayed after its first search';
ok $standby->log_contains(qr/chdb_search detach: DETACH TABLE IF EXISTS idx_\d+\.t_\d+ SYNC/),
    'The standby should put the table back when the pages change';
ok $standby->log_contains(qr/chdb_search attach: ATTACH TABLE IF NOT EXISTS idx_\d+\.t_\d+$/m),
    'By name, from the definition the engine kept';
ok !$standby->log_contains(qr/page request \d+ failed/),
    'The engine should have asked the standby for no write';

# A transaction on the primary that has staged rows and not committed: its
# staging table's blobs reach the standby's pages, but the standby attaches
# no staging table, as only that transaction ever reads its own.
my $oid      = $primary->safe_psql(postgres => "SELECT 'docs_idx'::regclass::oid");
my $inflight = $primary->background_psql('postgres');
$inflight->query_safe(q{
    SET chdb_search.flush_threshold = '64kB';
    BEGIN;
    INSERT INTO docs SELECT 1000 + i, 'staged ' || repeat('word ', 250) FROM generate_series(1, 120) i;
});
like store_tables($primary, $oid), qr/^t_\d+_tx_\d+$/m, 'The primary should have staged the rows';
$primary->wait_for_catchup($standby);
is search($standby), "2\n3\n4", 'The standby should answer as before';
ok !$standby->log_contains(qr/chdb_search attach: ATTACH TABLE IF NOT EXISTS idx_\d+\.t_\d+_tx_\d+/),
    'The standby should attach no staging table';
$inflight->query_safe('ROLLBACK');
$inflight->quit;
$primary->wait_for_catchup($standby);

# Promoted, the standby serves the index at once from the same pages, with
# an engine restarted read-write, and takes rows.
my $offset = -s $standby->logfile;
$standby->promote;
$standby->poll_query_until(postgres => 'SELECT NOT pg_is_in_recovery()')
    or die 'the standby did not promote';
$standby->wait_for_log(qr/promoted, the engine restarts read-write/, $offset);
is search($standby), "2\n3\n4", 'The promoted standby should answer with every row';
$standby->safe_psql(postgres => "INSERT INTO docs VALUES (5, 'Riding boots')");
is search($standby), "2\n3\n4\n5", 'The promoted standby should take new rows';
ok $standby->log_contains(
    qr/chdb_search attach: ATTACH TABLE IF NOT EXISTS idx_\d+\.t_\d+ UUID (?!.*table_readonly).*$/m, $offset),
    'The new engine should attach the table read-write';
ok !$standby->log_contains(qr/has no store|does not match its store/, 0),
    'Nothing should have asked for a REINDEX';
$standby->stop;

# The engine's directory on the primary is a cache: removed, it is rebuilt.
$primary->stop;
ok -d $primary->data_dir . $cache, 'The primary should have an engine directory';
rmtree($primary->data_dir . $cache);
$primary->start;
is search($primary), "2\n3\n4", 'The index should answer without its engine directory';
ok -d $primary->data_dir . $cache, 'The worker should have made the directory again';

# A REINDEX is never needed, but still works.
$primary->safe_psql(postgres => 'REINDEX INDEX docs_idx');
is search($primary), "2\n3\n4", 'REINDEX should keep the index answering';

done_testing;
