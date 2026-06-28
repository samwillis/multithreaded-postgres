# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('phase16b_shell_pool_state_isolation');

sub wait_for_pid_to_leave_pg_stat_activity
{
	my ($pid, $label) = @_;

	$node->poll_query_until(
		'postgres',
		"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid = $pid);",
		't') || die "timed out waiting for $label";
	pass($label);
}

sub wait_for_validation_destroy_log
{
	my ($label) = @_;
	my $log = '';

	for (1 .. 100)
	{
		$log = slurp_file($node->logfile);
		foreach my $line (split /\n/, $log)
		{
			next unless $line =~ /threaded_session_pool_validation/;
			next unless $line =~ /action=destroy/;
			next unless $line =~ /reason=ok/;
			next unless $line =~ /reason_count=([1-9]\d*)/;

			pass($label);
			return;
		}
		usleep(100_000);
	}

	fail($label);
	diag("server log did not contain validation destroy accounting:\n$log");
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
	'shell state isolation test starts threaded runtime');
is($node->safe_psql('postgres', 'SHOW threaded_session_pool'), 'shell',
	'shell pool mode is configured');
is($node->safe_psql('postgres', 'SHOW threaded_session_pool_max'), '1',
	'shell pool carrier cache limit is configured');

my $default_work_mem = $node->safe_psql('postgres', 'SHOW work_mem');

# Seed one idle shell carrier before running the dirty session.
is($node->safe_psql('postgres', 'SELECT 16031;'), '16031',
	'shell pool accepts seed connection');
usleep(200_000);

my $dirty = $node->background_psql('postgres', timeout => 30);
my $dirty_pid = $dirty->query_safe('SELECT pg_backend_pid();', verbose => 0);

$dirty->query_safe(q{SET work_mem = '64MB';}, verbose => 0);
$dirty->query_safe(q{CREATE TEMP TABLE phase16b_temp(x int);}, verbose => 0);
$dirty->query_safe(q{INSERT INTO phase16b_temp VALUES (1);}, verbose => 0);
$dirty->query_safe(q{PREPARE phase16b_stmt AS SELECT 42;}, verbose => 0);
$dirty->query_safe(q{LISTEN phase16b_channel;}, verbose => 0);
$dirty->query_safe(q{SELECT pg_advisory_lock(160013);}, verbose => 0);

is($dirty->query_safe(
		q{SELECT to_regclass('pg_temp.phase16b_temp') IS NOT NULL;},
		verbose => 0),
	't',
	'dirty session sees its temp table before disconnect');
is($dirty->query_safe(
		q{SELECT count(*) FROM pg_prepared_statements WHERE name = 'phase16b_stmt';},
		verbose => 0),
	'1',
	'dirty session sees its prepared statement before disconnect');
is($dirty->query_safe(
		q{SELECT count(*) FROM pg_listening_channels() WHERE pg_listening_channels = 'phase16b_channel';},
		verbose => 0),
	'1',
	'dirty session sees its LISTEN state before disconnect');

$dirty->quit;
wait_for_pid_to_leave_pg_stat_activity($dirty_pid, 'dirty shell session exits');

is($node->safe_psql(
		'postgres',
		q{SELECT to_regclass('pg_temp.phase16b_temp') IS NULL;}),
	't',
	'next client cannot see prior temp table');
is($node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_prepared_statements WHERE name = 'phase16b_stmt';}),
	'0',
	'next client cannot see prior prepared statement');
is($node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_listening_channels() WHERE pg_listening_channels = 'phase16b_channel';}),
	'0',
	'next client cannot see prior LISTEN state');
is($node->safe_psql('postgres', 'SHOW work_mem'), $default_work_mem,
	'next client sees default work_mem');

my $lock_check = $node->background_psql('postgres', timeout => 30);
is($lock_check->query_safe(q{SELECT pg_try_advisory_lock(160013);},
		verbose => 0),
	't',
	'prior session advisory lock was released');
is($lock_check->query_safe(q{SELECT pg_advisory_unlock(160013);},
		verbose => 0),
	't',
	'advisory lock cleanup succeeds');
$lock_check->quit;

wait_for_validation_destroy_log(
	'shell pool logs validation destroy action with reason accounting');

unlike(
	slurp_file($node->logfile),
	qr/PANIC|segmentation|server closed the connection unexpectedly|server process .* was terminated|was terminated by signal/,
	'shell state isolation log has no crash signatures');

$node->stop('fast');

done_testing();
