package chDBTestUtils;

use strict;
use warnings;
use Exporter 'import';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

our @EXPORT = qw(
    server_log check_log check_query search_node worker_pid stop_worker
    stderr_of store_tables
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
at DEBUG1 so that the statements the index sends to its worker show. Lines
for postgresql.conf follow the name.
The worker needs libchdb on the server's library path; with the stub client
(make CHDB_SEARCH_STUB=1, which the tests see in the environment) there is no
worker, and tests of what the store holds skip.

=cut

sub search_node {
    my ($name, @conf) = @_;
    my $node = PostgreSQL::Test::Cluster->new($name);
    $node->init;
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

1;
