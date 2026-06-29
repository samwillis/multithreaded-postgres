# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node =
  PostgreSQL::Test::Cluster->new('phase16b_shell_pool_buffer_pin_reset');

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

sub wait_for_validation_ok_for_pid
{
	my ($pid, $label) = @_;
	my $log = '';

	for (1 .. 100)
	{
		foreach my $line (validation_lines())
		{
			next unless $line =~ /pid=\Q$pid\E\b/;

			if ($line =~ /reason=buffer_pins/ || $line !~ /reason=ok/)
			{
				fail($label);
				diag("unexpected buffer validation result:\n$line");
				return;
			}

			next unless $line =~ /action=destroy/;
			next unless $line =~ /reusable=1/;

			pass($label);
			return;
		}

		$log = slurp_file($node->logfile);
		usleep(100_000);
	}

	fail($label);
	diag("server log did not contain validation success for pid $pid:\n$log");
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
	'buffer reset test starts threaded runtime');
is($node->safe_psql('postgres', 'SHOW threaded_session_pool'), 'shell',
	'shell pool mode is configured');
is($node->safe_psql('postgres', 'SHOW threaded_session_pool_max'), '1',
	'shell pool carrier cache limit is configured');

$node->safe_psql('postgres',
	q{CREATE EXTENSION test_backend_runtime_threaded;});
is($node->safe_psql(
		'postgres',
		q{SELECT test_backend_runtime_buffer_refcount_state_is_reusable();}),
	't',
	'buffer refcount predicate starts reusable');

my $workload = $node->background_psql('postgres', timeout => 30);
my $workload_pid = $workload->query_safe('SELECT pg_backend_pid();',
	verbose => 0);

$workload->query_safe(q{
CREATE TABLE phase16b_buffer_pin_source(x int PRIMARY KEY, payload text);
INSERT INTO phase16b_buffer_pin_source
SELECT g, repeat('buffer-reset-proof', 16)
FROM generate_series(1, 8000) AS g;
}, verbose => 0);

is($workload->query_safe(
		q{SELECT test_backend_runtime_buffer_refcount_state_is_reusable();},
		verbose => 0),
	't',
	'buffer refcount predicate is reusable after heap insert');

is($workload->query_safe(
		q{SELECT count(*) FROM phase16b_buffer_pin_source WHERE payload LIKE 'buffer%';},
		verbose => 0),
	'8000',
	'heap scan touched real shared buffers');
is($workload->query_safe(
		q{SELECT test_backend_runtime_buffer_refcount_state_is_reusable();},
		verbose => 0),
	't',
	'buffer refcount predicate is reusable after heap scan');

$workload->query_safe(q{SET enable_seqscan = off;}, verbose => 0);
is($workload->query_safe(
		q{SELECT count(*) FROM phase16b_buffer_pin_source WHERE x BETWEEN 100 AND 4000;},
		verbose => 0),
	'3901',
	'index scan touched real shared buffers');
is($workload->query_safe(
		q{SELECT test_backend_runtime_buffer_refcount_state_is_reusable();},
		verbose => 0),
	't',
	'buffer refcount predicate is reusable after index scan');
$workload->query_safe(q{RESET enable_seqscan;}, verbose => 0);

$workload->quit;
wait_for_pid_to_leave_pg_stat_activity($workload_pid,
	'buffer workload shell session exits');

wait_for_validation_ok_for_pid(
	$workload_pid,
	'shell validation succeeds after real buffer activity');

is($node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM phase16b_buffer_pin_source;}),
	'8000',
	'next client can read table after buffer workload');

$node->safe_psql('postgres', q{DROP TABLE phase16b_buffer_pin_source;});

unlike(
	slurp_file($node->logfile),
	qr/PANIC|segmentation|server closed the connection unexpectedly|server process .* was terminated|was terminated by signal/,
	'buffer reset log has no crash signatures');

$node->stop('fast');

done_testing();
