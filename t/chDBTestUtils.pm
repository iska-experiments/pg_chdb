package chDBTestUtils;

use strict;
use warnings;
use Exporter 'import';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

our @EXPORT = qw(server_log check_log check_query search_node worker_pid psql_retry);

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

The pid of the chdb_search worker for a database, waiting a while for one to
appear. Returns an empty string if none does.

=cut

sub worker_pid {
    my ($node, $db) = @_;
    my $pid = '';
    for (1 .. 200) {
        $pid = $node->safe_psql($db => q{SELECT pid FROM pg_stat_activity
            WHERE backend_type = 'chdb_search worker'
              AND datname = current_database()});
        last if $pid ne '';
        select undef, undef, undef, 0.05;
    }
    return $pid;
}

=head2 psql_retry

Run a query until it succeeds, as connections are refused while the instance
recovers from a crash. Returns psql's return code, stdout, and stderr.

=cut

sub psql_retry {
    my ($node, $db, $sql) = @_;
    my ($rc, $out, $err);
    for (1 .. 200) {
        ($rc, $out, $err) = $node->psql($db => $sql);
        last if $rc == 0;
        select undef, undef, undef, 0.1;
    }
    return ($rc, $out, $err);
}

1;
