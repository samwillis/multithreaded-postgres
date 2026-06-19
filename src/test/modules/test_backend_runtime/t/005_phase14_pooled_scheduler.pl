# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use IPC::Run ();
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('phase14_pooled_scheduler');

sub start_psql_script
{
	my ($sql, $timeout) = @_;
	my $stdin = $sql;
	my $stdout = '';
	my $stderr = '';
	my $timer = IPC::Run::timer($timeout);
	my @cmd = (
		'psql',
		'--no-psqlrc',
		'--no-align',
		'--tuples-only',
		'--quiet',
		'--dbname' => $node->connstr('postgres'),
		'--file' => '-');
	my $run = IPC::Run::start(\@cmd, '<', \$stdin, '>', \$stdout, '2>',
		\$stderr, $timer);

	return {
		run => $run,
		timer => $timer,
		stdout => \$stdout,
		stderr => \$stderr,
	};
}

sub finish_psql
{
	my ($psql) = @_;

	eval { $psql->{run}->finish; };
	return;
}

sub wait_for_completion_snapshot
{
	my ($pid, $pattern, $label) = @_;
	my $snapshot = '';

	for (1 .. 100)
	{
		$snapshot = $node->safe_psql(
			'postgres',
			"SELECT coalesce(test_backend_runtime_wait_completion_snapshot($pid), '');");
		if ($snapshot =~ $pattern)
		{
			pass($label);
			return $snapshot;
		}
		usleep(100_000);
	}

	fail($label);
	diag("last wait-completion snapshot for $pid: \"$snapshot\"");
	return $snapshot;
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
multithreaded_pooled_scheduler = on
autovacuum = off
io_method = sync
summarize_wal = off
log_min_messages = debug1
});
$node->start;

is($node->safe_psql('postgres', 'SHOW multithreaded'), 'on',
	'Phase 14 pooled scheduler smoke starts threaded runtime');
is($node->safe_psql('postgres', 'SHOW multithreaded_pooled_scheduler'),
	'on', 'Phase 14 pooled scheduler smoke enables pooled runtime');

$node->safe_psql('postgres',
	'CREATE EXTENSION test_backend_runtime_threaded;');
is($node->safe_psql(
		'postgres',
		'SELECT test_backend_runtime_current_runtime_kind();'),
	'pooled-scheduler',
	'Phase 14 pooled scheduler smoke runs SQL in pooled runtime');

my $session = $node->background_psql('postgres', timeout => 30);
is($session->query_safe(
		'SELECT test_backend_runtime_current_runtime_kind();',
		verbose => 0),
	'pooled-scheduler',
	'background client also runs on pooled runtime');
my $pid = $session->query_safe('SELECT pg_backend_pid();', verbose => 0);

wait_for_completion_snapshot(
	$pid,
	qr/^waiting\|event_set\|ClientRead\|1\|.*\|1\|1\|1$/,
	'idle pooled client parks at scheduler-visible frontend input wait');

is($session->query_safe('SELECT 21 * 2;', verbose => 0),
	'42', 'pooled client resumes from frontend input wait');

wait_for_completion_snapshot(
	$pid,
	qr/^waiting\|event_set\|ClientRead\|1\|.*\|1\|1\|1$/,
	'pooled client parks again after resumed command');

my @sessions;
my @pids;
for my $i (1 .. 3)
{
	my $pooled = $node->background_psql('postgres', timeout => 30);
	my $pooled_pid = $pooled->query_safe('SELECT pg_backend_pid();',
		verbose => 0);

	push @sessions, $pooled;
	push @pids, $pooled_pid;
	wait_for_completion_snapshot(
		$pooled_pid,
		qr/^waiting\|event_set\|ClientRead\|1\|.*\|1\|1\|1$/,
		"pooled client $i publishes frontend input wait");
}

for my $i (0 .. $#sessions)
{
	is($sessions[$i]->query_safe('SELECT 100 + ' . ($i + 1) . ';',
			verbose => 0),
		101 + $i,
		"pooled client " . ($i + 1) . " resumes and runs another command");
}

$session->quit;
wait_for_pid_to_leave_pg_stat_activity($pid,
	'pooled client exits cleanly');
for my $i (0 .. $#sessions)
{
	$sessions[$i]->quit;
	wait_for_pid_to_leave_pg_stat_activity($pids[$i],
		"pooled client " . ($i + 1) . " exits cleanly");
}

my $write_psql = start_psql_script(
	"SELECT pg_backend_pid();\nCOPY (SELECT repeat('x', 65536) FROM generate_series(1, 20000)) TO STDOUT;\n",
	120);
ok(pump_until($write_psql->{run}, $write_psql->{timer},
		$write_psql->{stdout}, qr/^\d+\s*$/m),
	'pooled frontend-output wait backend reported logical backend id');
my ($write_pid) = ${ $write_psql->{stdout} } =~ /^(\d+)\s*$/m;

wait_for_completion_snapshot(
	$write_pid,
	qr/^waiting\|event_set\|ClientWrite\|1\|.*\|1\|1\|1$/,
	'pooled scheduler publishes frontend output wait under backpressure');

is($node->safe_psql(
		'postgres',
		"SELECT wait_event FROM pg_stat_activity WHERE pid = $write_pid;"),
	'ClientWrite',
	'pg_stat_activity reports pooled backend waiting on frontend output');

eval { $write_psql->{run}->kill_kill; };
pass('pooled frontend-output wait client can disconnect cleanly');
wait_for_pid_to_leave_pg_stat_activity($write_pid,
	'disconnected pooled frontend-output backend leaves pg_stat_activity');

my $sleep_psql = start_psql_script(
	"SELECT pg_backend_pid();\nSELECT pg_sleep(30);\n",
	30);
ok(pump_until($sleep_psql->{run}, $sleep_psql->{timer},
		$sleep_psql->{stdout}, qr/^\d+\s*$/m),
	'pooled latch wait backend reported logical backend id');
my ($sleep_pid) = ${ $sleep_psql->{stdout} } =~ /^(\d+)\s*$/m;

wait_for_completion_snapshot(
	$sleep_pid,
	qr/^waiting\|event_set\|PgSleep\|1\|.*\|1\|1\|1$/,
	'pooled scheduler publishes latch wait for pg_sleep');

is($node->safe_psql('postgres', "SELECT pg_cancel_backend($sleep_pid);"),
	't', 'pooled latch wait accepts query cancel');
ok(pump_until($sleep_psql->{run}, $sleep_psql->{timer},
		$sleep_psql->{stderr}, qr/canceling statement due to user request/),
	'pooled latch wait observes query cancel');
finish_psql($sleep_psql);
wait_for_pid_to_leave_pg_stat_activity($sleep_pid,
	'canceled pooled latch-wait backend leaves pg_stat_activity');

my $timeout_psql = start_psql_script(
	"SELECT pg_backend_pid();\nSET statement_timeout = '1s';\nSELECT pg_sleep(30);\n",
	30);
ok(pump_until($timeout_psql->{run}, $timeout_psql->{timer},
		$timeout_psql->{stdout}, qr/^\d+\s*$/m),
	'pooled timeout wait backend reported logical backend id');
my ($timeout_pid) = ${ $timeout_psql->{stdout} } =~ /^(\d+)\s*$/m;

wait_for_completion_snapshot(
	$timeout_pid,
	qr/^waiting\|event_set\|PgSleep\|1\|.*\|1\|1\|1$/,
	'pooled scheduler publishes wait before statement timeout fires');
ok(pump_until($timeout_psql->{run}, $timeout_psql->{timer},
		$timeout_psql->{stderr}, qr/canceling statement due to statement timeout/),
	'pooled scheduler delivers statement timeout while backend is waiting');
finish_psql($timeout_psql);
wait_for_pid_to_leave_pg_stat_activity($timeout_pid,
	'statement-timeout pooled backend leaves pg_stat_activity');

my $lock_holder = $node->background_psql('postgres', timeout => 30);
$lock_holder->query_safe('SELECT pg_advisory_lock(140014);', verbose => 0);

my $lock_psql = start_psql_script(
	"SELECT pg_backend_pid();\nSELECT pg_advisory_lock(140014);\n",
	60);
ok(pump_until($lock_psql->{run}, $lock_psql->{timer},
		$lock_psql->{stdout}, qr/^\d+\s*$/m),
	'pooled heavyweight-lock wait backend reported logical backend id');
my ($lock_pid) = ${ $lock_psql->{stdout} } =~ /^(\d+)\s*$/m;

wait_for_completion_snapshot(
	$lock_pid,
	qr/^waiting\|event_set\|advisory\|1\|.*\|1\|1\|1$/,
	'pooled scheduler publishes advisory lock wait');

is($node->safe_psql('postgres', "SELECT pg_cancel_backend($lock_pid);"),
	't', 'pooled advisory lock wait accepts query cancel');
ok(pump_until($lock_psql->{run}, $lock_psql->{timer},
		$lock_psql->{stderr}, qr/canceling statement due to user request/),
	'pooled advisory lock wait observes query cancel');
finish_psql($lock_psql);
wait_for_pid_to_leave_pg_stat_activity($lock_pid,
	'canceled pooled advisory-lock backend leaves pg_stat_activity');

$lock_holder->query_safe('SELECT pg_advisory_unlock(140014);', verbose => 0);
$lock_holder->quit;

my $stress_holder = $node->background_psql('postgres', timeout => 30);
$stress_holder->query_safe('SELECT pg_advisory_lock(140015);', verbose => 0);

my @stress_waiters;
my @stress_pids;
for my $i (1 .. 5)
{
	my $waiter = start_psql_script(
		"SELECT pg_backend_pid();\nSELECT pg_advisory_lock(140015);\nSELECT pg_advisory_unlock(140015);\nSELECT 'done';\n",
		90);

	ok(pump_until($waiter->{run}, $waiter->{timer},
			$waiter->{stdout}, qr/^\d+\s*$/m),
		"pooled lost-wakeup stress waiter $i reported logical backend id");
	my ($waiter_pid) = ${ $waiter->{stdout} } =~ /^(\d+)\s*$/m;
	push @stress_waiters, $waiter;
	push @stress_pids, $waiter_pid;

	wait_for_completion_snapshot(
		$waiter_pid,
		qr/^waiting\|event_set\|advisory\|1\|.*\|1\|1\|1$/,
		"pooled lost-wakeup stress waiter $i publishes advisory wait");
}

$stress_holder->query_safe('SELECT pg_advisory_unlock(140015);', verbose => 0);
$stress_holder->quit;

for my $i (0 .. $#stress_waiters)
{
	ok(pump_until($stress_waiters[$i]->{run}, $stress_waiters[$i]->{timer},
			$stress_waiters[$i]->{stdout}, qr/^done\s*$/m),
		"pooled lost-wakeup stress waiter " . ($i + 1) . " completes after release");
	finish_psql($stress_waiters[$i]);
	wait_for_pid_to_leave_pg_stat_activity($stress_pids[$i],
		"pooled lost-wakeup stress waiter " . ($i + 1) . " leaves pg_stat_activity");
}

is($node->safe_psql('postgres', 'SELECT 42;'), '42',
	'pooled scheduler server remains usable after Gate F wait stress');

$node->stop;

done_testing();
