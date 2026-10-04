#!/usr/bin/perl

# DROP INDEX CONCURRENTLY commits internally while the index is still
# indisready, so a writer that opened it before flushes to its store at its
# own commit. The store must outlive those commits and go once, after the
# last of them.

use v5.34;
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use chDBTestUtils;

plan skip_all => 'needs the chdb_search worker' if $ENV{CHDB_SEARCH_STUB};

# The dropping session needs the library for its hook to see the drop at all.
my $node = search_node('concurrent', "session_preload_libraries = 'chdb_search'");
END { $node->stop if $node }

my $oid = $node->safe_psql(postgres => "SELECT 'docs_idx'::regclass::oid");
my $offset = -s $node->logfile;

# s1 holds a buffered row for the index; s2's DROP waits for s1 to end.
my $s1 = $node->background_psql('postgres');
$s1->query_safe("BEGIN; INSERT INTO docs VALUES (3, 'hello shoes');");
my $s2 = $node->background_psql('postgres');
$s2->query_until(qr/start/, "\\echo start\nDROP INDEX CONCURRENTLY docs_idx;\n");
ok $node->poll_query_until(postgres => q{
    SELECT count(*) > 0 FROM pg_stat_activity
     WHERE query LIKE 'DROP INDEX CONCURRENTLY%' AND wait_event_type = 'Lock'
}), 'DROP INDEX CONCURRENTLY should wait for the writer';

# s1 commits: its flush finds the store, and only then does the drop finish.
$s1->query_safe('COMMIT;');
$s1->quit;
$s2->quit;
is $s2->{stderr}, '', 'DROP INDEX CONCURRENTLY should complete';
is $node->safe_psql(postgres => "SELECT count(*) FROM pg_class WHERE oid = $oid"), 0,
    'Should drop the index';
is $node->safe_psql(postgres => 'SELECT count(*) FROM docs WHERE id = 3'), 1,
    'Should commit the writer';

my @log = split /\n/, slurp_file($node->logfile, $offset);
my ($insert) = grep {
    $log[$_] =~ /chdb_search insert: INSERT INTO idx_$oid\.t_\d+ .* -- 1 rows/
} 0 .. $#log;
my @drops = grep { $log[$_] =~ /chdb_search drop: idx_$oid$/ } 0 .. $#log;
ok defined $insert, 'The writer should flush to the store';
is scalar @drops, 1, 'Should drop the store once';
ok defined $insert && @drops == 1 && $drops[0] > $insert,
    'Should drop the store after the writer flushed';
is $node->safe_psql(postgres => qq{
    SELECT * FROM chdb_search_query(
        'SELECT count() FROM system.databases WHERE name = ''idx_$oid'''
    ) AS (n bigint)
}), 0, 'The store should be gone';

done_testing;
