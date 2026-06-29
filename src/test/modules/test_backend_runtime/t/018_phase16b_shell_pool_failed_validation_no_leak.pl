# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node =
  PostgreSQL::Test::Cluster->new('phase16b_shell_pool_failed_validation_no_leak');

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

sub wait_for_quarantine_validation_for_pid
{
	my ($pid, $label) = @_;
	my $log = '';

	for (1 .. 100)
	{
		foreach my $line (validation_lines())
		{
			next unless $line =~ /pid=\Q$pid\E\b/;

			if ($line =~ /reason=ok/ || $line !~ /action=quarantine_destroy/)
			{
				fail($label);
				diag("unexpected validation result:\n$line");
				return;
			}

			next unless $line =~ /reusable=0/;
			next unless $line =~ /quarantine_destroy_paths=([1-9]\d*)/;
			next unless $line =~ /validation_failures=([1-9]\d*)/;

			pass($label);
			return;
		}

		$log = slurp_file($node->logfile);
		usleep(100_000);
	}

	fail($label);
	diag("server log did not contain quarantine validation for pid $pid:\n$log");
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
debug_threaded_session_pool_force_validation_failure = on
autovacuum = off
io_method = sync
summarize_wal = off
log_min_messages = debug1
});
$node->start;

is($node->safe_psql('postgres', 'SHOW multithreaded'), 'on',
	'failed-validation no-leak test starts threaded runtime');
is($node->safe_psql('postgres', 'SHOW threaded_session_pool'), 'shell',
	'shell pool mode is configured');
is($node->safe_psql(
		'postgres',
		'SHOW debug_threaded_session_pool_force_validation_failure'),
	'on',
	'forced validation failure is configured');

$node->safe_psql('postgres',
	q{CREATE EXTENSION test_backend_runtime_threaded;});
my $default_work_mem = $node->safe_psql('postgres', 'SHOW work_mem');

my $copy_file = $node->basedir . '/phase16b_failed_validation_copy.csv';
my $copy_file_sql = sql_literal($copy_file);

my $dirty = $node->background_psql('postgres', timeout => 60);
my $dirty_pid = $dirty->query_safe('SELECT pg_backend_pid();', verbose => 0);

$dirty->query_safe(q{SET work_mem = '64MB';}, verbose => 0);
$dirty->query_safe(q{
CREATE TEMP TABLE phase16b_failed_validation_temp(x int);
INSERT INTO phase16b_failed_validation_temp VALUES (1);
PREPARE phase16b_failed_validation_stmt AS SELECT 42;
LISTEN phase16b_failed_validation_channel;
SELECT pg_advisory_lock(160018);
CREATE TABLE phase16b_failed_validation_source(
	x int PRIMARY KEY,
	payload text
);
INSERT INTO phase16b_failed_validation_source
SELECT g, repeat('failed-validation-proof', 8)
FROM generate_series(1, 4000) AS g;
}, verbose => 0);

is($dirty->query_safe(
		q{SELECT to_regclass('pg_temp.phase16b_failed_validation_temp') IS NOT NULL;},
		verbose => 0),
	't',
	'dirty failed-validation session sees its temp table before disconnect');
is($dirty->query_safe(
		q{SELECT count(*) FROM pg_prepared_statements WHERE name = 'phase16b_failed_validation_stmt';},
		verbose => 0),
	'1',
	'dirty failed-validation session sees its prepared statement before disconnect');
is($dirty->query_safe(
		q{SELECT count(*) FROM pg_listening_channels()
		  WHERE pg_listening_channels = 'phase16b_failed_validation_channel';},
		verbose => 0),
	'1',
	'dirty failed-validation session sees its LISTEN state before disconnect');
is($dirty->query_safe(q{SHOW work_mem;}, verbose => 0),
	'64MB',
	'dirty failed-validation session sees its changed GUC before disconnect');

$dirty->query_safe(
	"COPY phase16b_failed_validation_source TO $copy_file_sql WITH (FORMAT csv);",
	verbose => 0);
$dirty->query_safe(q{TRUNCATE phase16b_failed_validation_source;},
	verbose => 0);
$dirty->query_safe(
	"COPY phase16b_failed_validation_source FROM $copy_file_sql WITH (FORMAT csv);",
	verbose => 0);
is($dirty->query_safe(
		q{SELECT count(*) FROM phase16b_failed_validation_source;},
		verbose => 0),
	'4000',
	'server-side COPY file round trip completed before failed validation');

like($dirty->query_safe(
		q{COPY (SELECT repeat('z', 1024) FROM generate_series(1, 32)) TO STDOUT;},
		verbose => 0),
	qr/zzzz/,
	'COPY TO STDOUT exercised real client socket before failed validation');

is($dirty->query_safe(
		q{SELECT count(*) FROM phase16b_failed_validation_source
		  WHERE payload LIKE 'failed-validation%';},
		verbose => 0),
	'4000',
	'heap scan touched real shared buffers before failed validation');
$dirty->query_safe(q{SET enable_seqscan = off;}, verbose => 0);
is($dirty->query_safe(
		q{SELECT count(*) FROM phase16b_failed_validation_source
		  WHERE x BETWEEN 100 AND 3000;},
		verbose => 0),
	'2901',
	'index scan touched real shared buffers before failed validation');
$dirty->query_safe(q{RESET enable_seqscan;}, verbose => 0);
is($dirty->query_safe(
		q{SELECT test_backend_runtime_buffer_refcount_state_is_reusable();},
		verbose => 0),
	't',
	'buffer refcount predicate is clean before forced failed validation');

$dirty->quit;
wait_for_pid_to_leave_pg_stat_activity($dirty_pid,
	'dirty failed-validation shell session exits');
wait_for_quarantine_validation_for_pid(
	$dirty_pid,
	'shell pool quarantines failed validation after broad real workload');

is($node->safe_psql(
		'postgres',
		q{SELECT to_regclass('pg_temp.phase16b_failed_validation_temp') IS NULL;}),
	't',
	'next client cannot see temp table after failed validation');
is($node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_prepared_statements
		  WHERE name = 'phase16b_failed_validation_stmt';}),
	'0',
	'next client cannot see prepared statement after failed validation');
is($node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_listening_channels()
		  WHERE pg_listening_channels = 'phase16b_failed_validation_channel';}),
	'0',
	'next client cannot see LISTEN state after failed validation');
is($node->safe_psql('postgres', 'SHOW work_mem'), $default_work_mem,
	'next client sees default work_mem after failed validation');
is($node->safe_psql(
		'postgres',
		q{SELECT test_backend_runtime_buffer_refcount_state_is_reusable();}),
	't',
	'next client sees clean buffer refcount state after failed validation');
is($node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM phase16b_failed_validation_source;}),
	'4000',
	'next client can read persistent table after failed validation');

my $lock_check = $node->background_psql('postgres', timeout => 30);
is($lock_check->query_safe(q{SELECT pg_try_advisory_lock(160018);},
		verbose => 0),
	't',
	'failed-validation advisory lock was released');
is($lock_check->query_safe(q{SELECT pg_advisory_unlock(160018);},
		verbose => 0),
	't',
	'advisory lock cleanup succeeds after failed validation');
$lock_check->quit;

$node->safe_psql('postgres',
	q{DROP TABLE phase16b_failed_validation_source;});
unlink $copy_file;

unlike(
	slurp_file($node->logfile),
	qr/PANIC|segmentation|server closed the connection unexpectedly|server process .* was terminated|was terminated by signal/,
	'failed-validation no-leak log has no crash signatures');

$node->stop('fast');

done_testing();
