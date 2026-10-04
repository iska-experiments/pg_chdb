package chDBTestUtils;

use strict;
use warnings;
use Exporter 'import';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

our @EXPORT = qw(
    server_log check_log check_query search_node worker_pid worker_socket stderr_of stop_worker
    store_tables store_rows search_ids check_unavailable pitr_rows check_restored
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

=head2 worker_socket

The name the chdb_search worker of database $db listens on, as
/proc/net/unix shows it: an abstract one, `@pg_chdb/<hash>/<dboid>`, on
Linux. '' when the worker holds no listening socket.

=cut

sub worker_socket {
    my ($node, $db) = @_;
    my $pid = worker_pid($node, $db) or return '';
    my %fds = map { (readlink($_) // '') =~ /^socket:\[(\d+)\]$/ ? ($1 => 1) : () }
        glob "/proc/$pid/fd/*";
    open my $unix, '<', '/proc/net/unix' or return '';
    while (<$unix>) {
        # Num RefCount Protocol Flags Type St Inode Path; a listener's flags
        # have __SO_ACCEPTCON.
        my @f = split;
        return $f[7] if @f == 8 && $fds{ $f[6] } && hex($f[3]) & 0x10000;
    }
    return '';
}

=head2 stop_worker

Stops the chdb_search worker of database $db and waits for it to be gone;
the next request starts a new one.

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

=head2 store_rows

The rows of docs_idx's store table, those of dead heap tuples included.

=cut

sub store_rows {
    my $node = shift;
    return $node->safe_psql(postgres => q{
        SELECT * FROM chdb_search_query(
            'SELECT count() FROM ' || chdb_search_store_table('docs_idx')
        ) AS (n bigint)
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

=head2 pitr_rows

Commits a row on an archiving primary after its backup, names a restore
point, commits another, and waits for the WAL holding them to be archived,
returning the restore point's name. A restore to the point has the first
row and not the second, and an index whose store is the backup's copy has
a flush in its metapage that the store never saw.

=cut

sub pitr_rows {
    my $primary = shift;
    $primary->safe_psql(postgres => "INSERT INTO docs VALUES (3, 'Hiking boots')");
    $primary->safe_psql(postgres => "SELECT pg_create_restore_point('before_more')");
    $primary->safe_psql(postgres => "INSERT INTO docs VALUES (4, 'Climbing boots')");

    my $wal = $primary->safe_psql(postgres => 'SELECT pg_walfile_name(pg_switch_wal())');
    $primary->poll_query_until(postgres =>
        "SELECT last_archived_wal >= '$wal' FROM pg_stat_archiver")
        or die "WAL up to $wal was not archived";
    return 'before_more';
}

=head2 check_restored

Asserts what a node restored to pitr_rows' restore point shows: the heap
has the row committed before the point and not the one after, the index
refuses the store copied with the backup, which is a flush behind the
metapage, and REINDEX rebuilds it from the heap, after which searches match
the heap and the index takes new rows.

=cut

sub check_restored {
    my $node = shift;
    local $Test::Builder::Level = $Test::Builder::Level + 1;
    is $node->safe_psql(postgres => 'SELECT id FROM docs ORDER BY id'), "1\n2\n3",
        'The heap should be at the restore point';
    check_unavailable($node, "2\n3",
        qr/The store was last flushed at [0-9A-F]+\/[0-9A-F]+, the index at/,
        qr/REINDEX INDEX "docs_idx" rebuilds its store\./);
    $node->safe_psql(postgres => 'REINDEX INDEX docs_idx');
    is search_ids($node, 'boots'), "2\n3", 'REINDEX should rebuild the store from the heap';
    is search_ids($node, 'hiking'), 3,
        'The row committed before the restore point should be in the rebuilt store';
    is search_ids($node, 'climbing'), '', 'The row committed after it should not';
    $node->safe_psql(postgres => "INSERT INTO docs VALUES (5, 'Riding boots')");
    is search_ids($node, 'boots'), "2\n3\n5", 'The rebuilt index should take new rows';
}

1;
