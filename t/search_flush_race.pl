#!/usr/bin/perl

# Writers committing a row at a time, each commit a flush into the store,
# while readers search through the index: no flush and no search may fail,
# every search answers, and the index ends up holding every committed row.
# This once pinned a race between the fail-safe check and a flush under a
# lock on the metapage; the check is gone with the store in the index's own
# pages, and the test stays as the one that runs the two paths against each
# other.

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

my $node = search_node('race');
END { $node->stop if $node }

my ($writers, $readers, $rounds) = (3, 3, 150);

# A psql running `$sql` statement by statement, stopping at the first error.
# The script is a file: a harness feeds a child's stdin only while pumped,
# which would run the sessions one after another.
my $dir = PostgreSQL::Test::Utils::tempdir;
sub run_sql {
    my ($name, $sql, $out, $err) = @_;
    my $file = "$dir/$name.sql";
    open my $fh, '>', $file or die "$file: $!";
    print $fh $sql;
    close $fh;
    return start [
        'psql', '-X', '-qtA', '-v', 'ON_ERROR_STOP=1', '-d', $node->connstr('postgres'),
        '-f', $file
    ], \undef, $out, $err;
}

# Every writer commits a row at a time, each commit a flush and a check, while
# every reader searches through the index, each search a check once a flush
# has moved the metapage on.
my (@procs, @out, @err);
for my $w (1 .. $writers) {
    push @out, ''; push @err, '';
    push @procs, run_sql("writer$w",
        join('', map { "INSERT INTO docs VALUES ($w * 1000 + $_, 'walking boots $_');\n" }
            1 .. $rounds),
        \$out[-1], \$err[-1]);
}
for my $r (1 .. $readers) {
    push @out, ''; push @err, '';
    push @procs, run_sql("reader$r",
        "SET enable_seqscan = off;\n"
            . ("SELECT count(*) FROM docs WHERE body @@@ 'boots';\n" x $rounds),
        \$out[-1], \$err[-1]);
}
while (my @live = grep { $_->pumpable } @procs) {
    $_->pump_nb for @live;
    select undef, undef, undef, 0.01;
}
$_->finish for @procs;

for my $i (0 .. $#procs) {
    my $who = $i < $writers ? 'Writer ' . ($i + 1) : 'Reader ' . ($i - $writers + 1);
    is $err[$i], '', "$who should see no error";
    is $procs[$i]->result(0), 0, "$who should finish every statement";
}
my @counts = map { split /\n/ } @out[$writers .. $#out];
is scalar @counts, $readers * $rounds, 'Every search should answer';
ok !(grep { $_ < 1 } @counts), 'No search should come back empty';
is $node->safe_psql(postgres =>
    "SET enable_seqscan = off; SELECT count(*) FROM docs WHERE body @@@ 'boots'"),
    1 + $writers * $rounds, 'The index should hold every committed row';
ok !$node->log_contains(qr/not available on this server/, 0),
    'Nothing should have been refused';

done_testing;
