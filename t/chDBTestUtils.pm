package chDBTestUtils;

use strict;
use warnings;
use Exporter 'import';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

our @EXPORT = qw(
    server_log check_log check_query search_node worker_pid stop_worker
    stderr_of store_tables search_ids check_unavailable
);

=begin server_log

Fetch server log lines since the last fetch.

=cut

{
    my $offset = 0;
    sub server_log($) {
        my $node = shift;
        my $data = slurp_file $node->logfile, $offset;
        $offset += length $data;
        return split /\n/, $data
    }
}

=head2 check_log

Compare the log lines immediately following an "executing chDB query" log line.
The first should contain the chDB query with placeholders. The second should map
the placeholders to values.

=cut

sub check_log {
    my ($file, $desc, $query_rx, $params_rx) = @_;
    my @lines = server_log $file;
    while (@lines && $lines[0] !~ /executing chDB query/) {
        shift @lines;
    }

    shift @lines;
    splice @lines, 2;
    is @lines, 2, "Should have 2 $desc log lines" || return;
    like $lines[0], $query_rx, "Should match $desc query";
    like $lines[1], $params_rx, "Should match $desc params";
}

=head2 check_query

Run a subtest to validate that a given query's log values containing the
resulting chDB query and associated parameters.

=cut

sub check_query {
    my ($node, $desc, $query, $err_rx, @args) = @_;
    subtest $desc => sub {
        eval { $node->safe_psql(postgres => $query) };
        like $@, $err_rx, "Should have $desc chDB error";
        check_log $node, $desc, @args;
    };
}

=head2 search_node

A node for the chdb_search tests: the extension, whose schema chdb is on the
search path, and a table docs with a chdb index docs_idx on its body, logging
at DEBUG1 so that the statements the index sends to its worker show. A hash
reference after the name passes parameters to init, as { allows_streaming =>
1 } does for a node to be backed up or replicated; lines for postgresql.conf
follow.
The worker needs libchdb on the server's library path; with the stub client
(make CHDB_SEARCH_STUB=1, which the tests see in the environment) there is no
worker, and tests of what the store holds skip.

=cut

sub search_node {
    my ($name, @conf) = @_;
    my %init = ref $conf[0] eq 'HASH' ? %{ shift @conf } : ();
    my $node = PostgreSQL::Test::Cluster->new($name);
    $node->init(%init);
    $node->append_conf('postgresql.conf',
        join "\n", 'log_min_messages = debug1', "search_path = 'public, chdb'",
        @conf, '');
    $node->start;
    $node->safe_psql(postgres => q{
        CREATE EXTENSION chdb_search;
        CREATE TABLE docs (id int PRIMARY KEY, body text);
        INSERT INTO docs VALUES (1, 'Running shoes for runners'), (2, 'Walking boots');
        CREATE INDEX docs_idx ON docs USING chdb (body);
    });
    return $node;
}

=head2 worker_pid

The pid of the chdb_search worker for a database, waiting the test timeout
for one to appear. Returns an empty string if none does.

=cut

sub worker_pid {
    my ($node, $db) = @_;
    my $worker = q{FROM pg_stat_activity
        WHERE backend_type = 'chdb_search worker' AND datname = current_database()};
    $node->poll_query_until($db, "SELECT EXISTS (SELECT $worker)") or return '';
    return $node->safe_psql($db => "SELECT pid $worker");
}

=head2 stop_worker

Stops the chdb_search worker of database $db, which removes its socket, and
waits for it to be gone; the next request starts a new one.

=cut

sub stop_worker {
    my ($node, $db) = @_;
    my $worker = qq{FROM pg_stat_activity
        WHERE backend_type = 'chdb_search worker' AND datname = '$db'};
    $node->safe_psql(postgres => "SELECT pg_terminate_backend(pid) $worker");
    $node->poll_query_until(postgres => "SELECT count(*) = 0 $worker")
        or die "the worker of $db did not stop";
}

=head2 stderr_of

The stderr of a statement run in the node's postgres database, '' when it
succeeds.

=cut

sub stderr_of {
    my ($node, $sql) = @_;
    my ($out, $err) = ('', '');
    $node->psql(postgres => $sql, stdout => \$out, stderr => \$err);
    return $err;
}

=head2 store_tables

The tables of index $oid's store, one name per line, read through the worker.

=cut

sub store_tables {
    my ($node, $oid) = @_;
    return $node->safe_psql(postgres => qq{
        SELECT * FROM chdb_search_query(
            'SELECT name FROM system.tables WHERE database = ''idx_$oid'' ORDER BY name'
        ) AS (name text)
    });
}

=head2 search_ids

The ids of the docs whose body has every token of $needle, one per line,
found through the index: sequential scans are disabled, so the answer is
the store's, filtered by the heap for visibility.

=cut

sub search_ids {
    my ($node, $needle) = @_;
    return $node->safe_psql(postgres => qq{
        SET enable_seqscan = off;
        SELECT id FROM docs WHERE body @@@ '$needle' ORDER BY id
    });
}

=head2 check_unavailable

Asserts that the node refuses docs_idx as chdb_search.unavailable_index
says: in error mode a search through the index fails naming the index, with
a DETAIL matching $detail and a HINT matching $hint; in skip mode the
planner takes a sequential scan, and the search answers $expect, the ids of
the rows with the token boots, from the heap.

=cut

sub check_unavailable {
    my ($node, $expect, $detail, $hint) = @_;
    local $Test::Builder::Level = $Test::Builder::Level + 1;
    my $search = "SELECT id FROM docs WHERE body @@@ 'boots'";
    my $skip   = 'SET chdb_search.unavailable_index = skip;';
    my $err    = stderr_of($node, "SET enable_seqscan = off; $search");
    like $err, qr/ERROR:\s+chdb index "docs_idx" is not available on this server/,
        'The index should be refused';
    like $err, qr/DETAIL:\s+$detail/, 'Should say why';
    like $err, qr/HINT:\s+$hint/, 'Should say what to do';
    like $node->safe_psql(postgres => "$skip EXPLAIN (COSTS OFF) $search"),
        qr/Seq Scan/, 'Skip mode should plan without the index';
    is $node->safe_psql(postgres => "$skip $search ORDER BY id"), $expect,
        'Skip mode should answer from the heap';
}

1;
