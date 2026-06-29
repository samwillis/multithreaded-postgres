# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node =
  PostgreSQL::Test::Cluster->new('phase16b_shell_pool_socket_fd_no_leak');

sub sql_literal
{
	my ($value) = @_;

	$value =~ s/'/''/g;
	return "'$value'";
}

sub validation_lines
{
	my @lines;
	my $log = slurp_file($node->logfile);

	foreach my $line (split /\n/, $log)
	{
		next unless $line =~ /threaded_session_pool_validation/;
		push @lines, $line;
	}

	return @lines;
}

sub wait_for_validation_ok_after
{
	my ($existing_count, $label) = @_;
	my $log = '';

	for (1 .. 100)
	{
		my @lines = validation_lines();
		my @new_lines = @lines[$existing_count .. $#lines];

		foreach my $line (@new_lines)
		{
			if ($line =~ /reason=(?:storage_state|socket_attached)/)
			{
				fail($label);
				diag("unexpected socket/FD validation failure:\n$line");
				return;
			}

			next unless $line =~ /action=destroy/;
			next unless $line =~ /reusable=1/;
			next unless $line =~ /reason=ok/;

			pass($label);
			return;
		}

		$log = slurp_file($node->logfile);
		usleep(100_000);
	}

	fail($label);
	diag("server log did not contain validation success after real socket/FD activity:\n$log");
}

sub wait_for_pid_to_leave_pg_stat_activity
{
	my ($pid, $label) = @_;

	$node->poll_query_until(
		'postgres',
		"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid = $pid);",
		't') || die "timed out waiting for $label";
	pass($label);
}

$node->init;
$node->append_conf(
	'postgresql.conf', q{
multithreaded = on
threaded_session_pool = shell
threaded_session_pool_max = 1
pooled_protocol_carriers = 0
log_threaded_lifecycle_timing = on
autovacuum = off
io_method = sync
summarize_wal = off
log_min_messages = debug1
});
$node->start;

is($node->safe_psql('postgres', 'SHOW multithreaded'), 'on',
	'socket/FD no-leak test starts threaded runtime');
is($node->safe_psql('postgres', 'SHOW threaded_session_pool'), 'shell',
	'shell pool mode is configured');
is($node->safe_psql('postgres', 'SHOW threaded_session_pool_max'), '1',
	'shell pool carrier cache limit is configured');

# Seed one shell carrier and wait until its validation has been logged, so the
# later log delta belongs to the real socket/FD workload session.
is($node->safe_psql('postgres', 'SELECT 16051;'), '16051',
	'shell pool accepts seed connection');
wait_for_validation_ok_after(0, 'seed shell validation succeeds');
my $validation_count = scalar validation_lines();

my $copy_file = $node->basedir . '/phase16b_socket_fd_copy.csv';
my $copy_file_sql = sql_literal($copy_file);

my $workload = $node->background_psql('postgres', timeout => 30);
my $workload_pid = $workload->query_safe('SELECT pg_backend_pid();',
	verbose => 0);

$workload->query_safe(q{
CREATE TABLE phase16b_socket_fd_source(x int, payload text);
INSERT INTO phase16b_socket_fd_source
SELECT g, repeat('x', 128)
FROM generate_series(1, 2000) AS g;
}, verbose => 0);

$workload->query_safe(
	"COPY phase16b_socket_fd_source TO $copy_file_sql WITH (FORMAT csv);",
	verbose => 0);
$workload->query_safe(q{TRUNCATE phase16b_socket_fd_source;}, verbose => 0);
$workload->query_safe(
	"COPY phase16b_socket_fd_source FROM $copy_file_sql WITH (FORMAT csv);",
	verbose => 0);

is($workload->query_safe(q{SELECT count(*) FROM phase16b_socket_fd_source;},
		verbose => 0),
	'2000',
	'server-side COPY file round trip completed');

like($workload->query_safe(
		q{COPY (SELECT repeat('y', 2048) FROM generate_series(1, 64)) TO STDOUT;},
		verbose => 0),
	qr/yyyy/,
	'COPY TO STDOUT exercised real client socket output');

$workload->quit;
wait_for_pid_to_leave_pg_stat_activity($workload_pid,
	'socket/FD workload shell session exits');

wait_for_validation_ok_after(
	$validation_count,
	'shell validation succeeds after real socket/FD activity');

is($node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM phase16b_socket_fd_source;}),
	'2000',
	'next client can read table after socket/FD workload');

$node->safe_psql('postgres', q{DROP TABLE phase16b_socket_fd_source;});
unlink $copy_file;

unlike(
	slurp_file($node->logfile),
	qr/PANIC|segmentation|server closed the connection unexpectedly|server process .* was terminated|was terminated by signal/,
	'socket/FD no-leak log has no crash signatures');

$node->stop('fast');

done_testing();
