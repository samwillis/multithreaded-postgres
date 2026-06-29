# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('phase16b_shell_pool_quarantine_destroy');

sub wait_for_pid_to_leave_pg_stat_activity
{
	my ($pid, $label) = @_;

	$node->poll_query_until(
		'postgres',
		"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid = $pid);",
		't') || die "timed out waiting for $label";
	pass($label);
}

sub quarantine_validation_lines
{
	my @lines;
	my $log = slurp_file($node->logfile);

	foreach my $line (split /\n/, $log)
	{
		next unless $line =~ /threaded_session_pool_validation/;
		next unless $line =~ /action=quarantine_destroy/;
		next unless $line =~ /reusable=0/;
		next unless $line =~ /reason=\w+/;
		next unless $line =~ /reason_count=([1-9]\d*)/;
		next unless $line =~ /quarantine_destroy_paths=([1-9]\d*)/;
		next unless $line =~ /validation_failures=([1-9]\d*)/;

		push @lines, $line;
	}

	return @lines;
}

sub wait_for_new_quarantine_validation_log
{
	my ($existing_count, $label) = @_;
	my $log = '';

	for (1 .. 100)
	{
		my @lines = quarantine_validation_lines();

		if (@lines > $existing_count)
		{
			pass($label);
			return;
		}

		$log = slurp_file($node->logfile);
		usleep(100_000);
	}

	fail($label);
	diag("server log did not contain new quarantine validation accounting:\n$log");
}

$node->init;
$node->append_conf(
	'postgresql.conf', q{
multithreaded = on
threaded_session_pool = shell
threaded_session_pool_max = 1
pooled_protocol_carriers = 0
log_threaded_lifecycle_timing = on
debug_threaded_session_pool_force_validation_failure = on
autovacuum = off
io_method = sync
summarize_wal = off
log_min_messages = debug1
});
$node->start;

is($node->safe_psql('postgres', 'SHOW multithreaded'), 'on',
	'quarantine test starts threaded runtime');
is($node->safe_psql('postgres', 'SHOW threaded_session_pool'), 'shell',
	'shell pool mode is configured');
is($node->safe_psql(
		'postgres',
		'SHOW debug_threaded_session_pool_force_validation_failure'),
	'on',
	'forced validation failure is configured');

my $default_work_mem = $node->safe_psql('postgres', 'SHOW work_mem');

is($node->safe_psql('postgres', 'SELECT 16041;'), '16041',
	'shell pool accepts seed connection');
usleep(200_000);

my @existing_quarantine_lines = quarantine_validation_lines();

my $dirty = $node->background_psql('postgres', timeout => 30);
my $dirty_pid = $dirty->query_safe('SELECT pg_backend_pid();', verbose => 0);

$dirty->query_safe(q{SET work_mem = '64MB';}, verbose => 0);
$dirty->query_safe(q{CREATE TEMP TABLE phase16b_quarantine_temp(x int);},
	verbose => 0);
$dirty->query_safe(q{INSERT INTO phase16b_quarantine_temp VALUES (1);},
	verbose => 0);
$dirty->query_safe(q{PREPARE phase16b_quarantine_stmt AS SELECT 42;},
	verbose => 0);
$dirty->query_safe(q{LISTEN phase16b_quarantine_channel;}, verbose => 0);
$dirty->query_safe(q{SELECT pg_advisory_lock(160014);}, verbose => 0);

is($dirty->query_safe(
		q{SELECT to_regclass('pg_temp.phase16b_quarantine_temp') IS NOT NULL;},
		verbose => 0),
	't',
	'dirty quarantine session sees its temp table before disconnect');
is($dirty->query_safe(
		q{SELECT count(*) FROM pg_prepared_statements WHERE name = 'phase16b_quarantine_stmt';},
		verbose => 0),
	'1',
	'dirty quarantine session sees its prepared statement before disconnect');

$dirty->quit;
wait_for_pid_to_leave_pg_stat_activity($dirty_pid,
	'dirty quarantine shell session exits');

wait_for_new_quarantine_validation_log(
	scalar @existing_quarantine_lines,
	'shell pool records quarantine destroy for failed validation');

is($node->safe_psql(
		'postgres',
		q{SELECT to_regclass('pg_temp.phase16b_quarantine_temp') IS NULL;}),
	't',
	'next client cannot see quarantined temp table');
is($node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_prepared_statements WHERE name = 'phase16b_quarantine_stmt';}),
	'0',
	'next client cannot see quarantined prepared statement');
is($node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_listening_channels() WHERE pg_listening_channels = 'phase16b_quarantine_channel';}),
	'0',
	'next client cannot see quarantined LISTEN state');
is($node->safe_psql('postgres', 'SHOW work_mem'), $default_work_mem,
	'next client sees default work_mem after quarantine destroy');

my $lock_check = $node->background_psql('postgres', timeout => 30);
is($lock_check->query_safe(q{SELECT pg_try_advisory_lock(160014);},
		verbose => 0),
	't',
	'quarantined session advisory lock was released');
is($lock_check->query_safe(q{SELECT pg_advisory_unlock(160014);},
		verbose => 0),
	't',
	'advisory lock cleanup succeeds after quarantine destroy');
$lock_check->quit;

unlike(
	slurp_file($node->logfile),
	qr/PANIC|segmentation|server closed the connection unexpectedly|server process .* was terminated|was terminated by signal/,
	'quarantine destroy log has no crash signatures');

$node->stop('fast');

done_testing();
