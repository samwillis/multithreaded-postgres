# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use IPC::Run ();
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node =
  PostgreSQL::Test::Cluster->new('phase16_extension_failure_paths');

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
autovacuum = off
io_method = sync
summarize_wal = off
log_min_messages = debug1
});
$node->start;

is($node->safe_psql('postgres', 'SHOW multithreaded'), 'on',
	'Phase 16 extension failure-path TAP starts threaded runtime');

my ($missing_ret, $missing_stdout, $missing_stderr) = $node->psql(
	'postgres',
	"LOAD 'test_backend_runtime_missing_phase16';",
	on_error_stop => 1);
isnt($missing_ret, 0, 'missing threaded module LOAD fails');
like(
	$missing_stderr,
	qr/could not access file|could not load library/,
	'missing threaded module LOAD reports loader failure');
is($node->safe_psql('postgres', 'SELECT 42;'), '42',
	'threaded server remains usable after missing module LOAD');

for my $attempt (1 .. 2)
{
	my ($ret, $stdout, $stderr) = $node->psql(
		'postgres',
		"LOAD 'test_backend_runtime_threaded_initfail';",
		on_error_stop => 1);
	isnt($ret, 0, "failing _PG_init LOAD attempt $attempt fails");
	like(
		$stderr,
		qr/test_backend_runtime requested _PG_init failure/,
		"failing _PG_init LOAD attempt $attempt reports module init failure");
}
is($node->safe_psql('postgres', 'SELECT 42;'), '42',
	'threaded server remains usable after repeated _PG_init failure');

$node->safe_psql('postgres',
	'CREATE EXTENSION test_backend_runtime_threaded;');
is($node->safe_psql(
		'postgres',
		q{
SELECT test_backend_runtime_custom_guc_value();
SELECT test_backend_runtime_custom_guc_init_count() >= 1;
}),
	"default\nt",
	'threaded helper module loads after failed module initialization');

my ($error_ret, $error_stdout, $error_stderr) = $node->psql(
	'postgres',
	q{
LOAD 'test_backend_runtime_threaded';
SET test_backend_runtime_threaded.custom_guc = 'before-error';
BEGIN;
SET LOCAL test_backend_runtime_threaded.custom_guc = 'local-before-error';
SELECT current_setting('test_backend_runtime_threaded.custom_guc');
SELECT test_backend_runtime_wait_on_condition_variable(-1);
ROLLBACK;
SHOW test_backend_runtime_threaded.custom_guc;
},
	on_error_stop => 0);
is($error_ret, 0,
	'psql continued after expected extension ERROR with ON_ERROR_STOP off');
like(
	$error_stderr,
	qr/condition variable wait timeout must not be negative/,
	'extension ERROR is reported to the client');
like(
	$error_stdout,
	qr/local-before-error.*before-error/s,
	'custom GUC stack is restored after extension ERROR and ROLLBACK');
is($node->safe_psql('postgres', 'SELECT 42;'), '42',
	'threaded server remains usable after extension ERROR');

my $cancel_psql = start_psql_script(
	"LOAD 'test_backend_runtime_threaded';\nSELECT pg_backend_pid();\nSELECT test_backend_runtime_wait_on_condition_variable(60000);\n",
	90);
ok(pump_until($cancel_psql->{run}, $cancel_psql->{timer},
		$cancel_psql->{stdout}, qr/^\d+\s*$/m),
	'extension wait backend reported logical backend id');
my ($cancel_pid) = ${ $cancel_psql->{stdout} } =~ /^(\d+)\s*$/m;
is($node->safe_psql('postgres', "SELECT pg_cancel_backend($cancel_pid);"),
	't', 'cancel request accepted for active extension wait');
ok(pump_until($cancel_psql->{run}, $cancel_psql->{timer},
		$cancel_psql->{stderr}, qr/canceling statement due to user request/),
	'active extension wait observes query cancel');
eval { $cancel_psql->{run}->finish; };
wait_for_pid_to_leave_pg_stat_activity($cancel_pid,
	'canceled extension wait backend leaves pg_stat_activity');

my $fatal_psql = start_psql_script(
	"LOAD 'test_backend_runtime_threaded';\nSET test_backend_runtime_threaded.custom_guc = 'fatal-session';\nSHOW test_backend_runtime_threaded.custom_guc;\nSELECT pg_backend_pid();\nSELECT test_backend_runtime_emit_fatal();\n",
	45);
ok(pump_until($fatal_psql->{run}, $fatal_psql->{timer},
		$fatal_psql->{stdout}, qr/^fatal-session\s*$/m),
	'FATAL fixture set extension custom GUC before exit');
ok(pump_until($fatal_psql->{run}, $fatal_psql->{timer},
		$fatal_psql->{stdout}, qr/^\d+\s*$/m),
	'FATAL extension backend reported logical backend id');
my ($fatal_pid) = ${ $fatal_psql->{stdout} } =~ /^(\d+)\s*$/m;
ok(pump_until($fatal_psql->{run}, $fatal_psql->{timer},
		$fatal_psql->{stderr}, qr/test_backend_runtime requested FATAL/),
	'extension backend reported test FATAL');
eval { $fatal_psql->{run}->finish; };
wait_for_pid_to_leave_pg_stat_activity($fatal_pid,
	'FATAL extension backend leaves pg_stat_activity');
is($node->safe_psql(
		'postgres',
		q{
LOAD 'test_backend_runtime_threaded';
SHOW test_backend_runtime_threaded.custom_guc;
}),
	'default',
	'reconnected threaded session receives fresh extension custom GUC state');
is($node->safe_psql('postgres', 'SELECT 42;'), '42',
	'threaded server remains usable after extension FATAL and reconnect');

$node->stop('fast');

done_testing();
