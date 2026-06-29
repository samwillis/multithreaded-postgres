# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node =
  PostgreSQL::Test::Cluster->new('phase16b_shell_pool_same_key_guc_baseline');

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

			if ($line =~ /reason=guc_state/ || $line !~ /reason=ok/)
			{
				fail($label);
				diag("unexpected GUC validation result:\n$line");
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
	'same-key GUC baseline test starts threaded runtime');
is($node->safe_psql('postgres', 'SHOW threaded_session_pool'), 'shell',
	'shell pool mode is configured');
is($node->safe_psql('postgres', 'SHOW threaded_session_pool_max'), '1',
	'shell pool carrier cache limit is configured');

$node->safe_psql(
	'postgres', q{
CREATE ROLE phase16b_guc_role LOGIN;
CREATE DATABASE phase16b_guc_db OWNER phase16b_guc_role;
});

$node->safe_psql(
	'phase16b_guc_db', q{
CREATE EXTENSION test_backend_runtime_threaded;
ALTER DATABASE phase16b_guc_db SET work_mem = '3MB';
ALTER ROLE phase16b_guc_role SET statement_timeout = '7s';
ALTER ROLE phase16b_guc_role IN DATABASE phase16b_guc_db
  SET default_statistics_target = 77;
});

my $same_key_connstr =
  $node->connstr('phase16b_guc_db') .
  " user=phase16b_guc_role options='-c lock_timeout=8s'";
my $other_startup_connstr =
  $node->connstr('phase16b_guc_db') .
  " user=phase16b_guc_role options='-c lock_timeout=11s'";

my $same_key = $node->background_psql(
	'phase16b_guc_db',
	connstr => $same_key_connstr,
	timeout => 30);
my $same_key_pid = $same_key->query_safe('SELECT pg_backend_pid();',
	verbose => 0);

is($same_key->query_safe(q{SELECT current_database();}, verbose => 0),
	'phase16b_guc_db',
	'same-key connection uses expected database');
is($same_key->query_safe(q{SELECT current_user;}, verbose => 0),
	'phase16b_guc_role',
	'same-key connection uses expected role');
is($same_key->query_safe(q{SHOW work_mem;}, verbose => 0),
	'3MB',
	'same-key connection applies database GUC default');
is($same_key->query_safe(q{SHOW statement_timeout;}, verbose => 0),
	'7s',
	'same-key connection applies role GUC default');
is($same_key->query_safe(q{SHOW default_statistics_target;}, verbose => 0),
	'77',
	'same-key connection applies role-in-database GUC default');
is($same_key->query_safe(q{SHOW lock_timeout;}, verbose => 0),
	'8s',
	'same-key connection applies startup packet GUC option');
is($same_key->query_safe(
		q{SELECT test_backend_runtime_guc_reset_baseline_matches();},
		verbose => 0),
	't',
	'same-key role/database/startup GUC baseline initially matches');

$same_key->query_safe(q{SET work_mem = '4MB';}, verbose => 0);
is($same_key->query_safe(
		q{SELECT test_backend_runtime_guc_reset_baseline_matches();},
		verbose => 0),
	'f',
	'database-default GUC drift fails baseline');
$same_key->query_safe(q{RESET work_mem;}, verbose => 0);
is($same_key->query_safe(q{SHOW work_mem;}, verbose => 0),
	'3MB',
	'RESET restores database GUC default');
is($same_key->query_safe(
		q{SELECT test_backend_runtime_guc_reset_baseline_matches();},
		verbose => 0),
	't',
	'database-default baseline matches after RESET');

$same_key->query_safe(q{SET statement_timeout = '9s';}, verbose => 0);
is($same_key->query_safe(
		q{SELECT test_backend_runtime_guc_reset_baseline_matches();},
		verbose => 0),
	'f',
	'role-default GUC drift fails baseline');
$same_key->query_safe(q{RESET statement_timeout;}, verbose => 0);
is($same_key->query_safe(q{SHOW statement_timeout;}, verbose => 0),
	'7s',
	'RESET restores role GUC default');
is($same_key->query_safe(
		q{SELECT test_backend_runtime_guc_reset_baseline_matches();},
		verbose => 0),
	't',
	'role-default baseline matches after RESET');

$same_key->query_safe(q{SET default_statistics_target = 88;}, verbose => 0);
is($same_key->query_safe(
		q{SELECT test_backend_runtime_guc_reset_baseline_matches();},
		verbose => 0),
	'f',
	'role-in-database GUC drift fails baseline');
$same_key->query_safe(q{RESET default_statistics_target;}, verbose => 0);
is($same_key->query_safe(q{SHOW default_statistics_target;}, verbose => 0),
	'77',
	'RESET restores role-in-database GUC default');
is($same_key->query_safe(
		q{SELECT test_backend_runtime_guc_reset_baseline_matches();},
		verbose => 0),
	't',
	'role-in-database baseline matches after RESET');

$same_key->query_safe(q{SET lock_timeout = '9s';}, verbose => 0);
is($same_key->query_safe(
		q{SELECT test_backend_runtime_guc_reset_baseline_matches();},
		verbose => 0),
	'f',
	'startup-packet GUC drift fails baseline');
$same_key->query_safe(q{RESET lock_timeout;}, verbose => 0);
is($same_key->query_safe(q{SHOW lock_timeout;}, verbose => 0),
	'8s',
	'RESET restores startup packet GUC option');
is($same_key->query_safe(
		q{SELECT test_backend_runtime_guc_reset_baseline_matches();},
		verbose => 0),
	't',
	'startup-packet baseline matches after RESET');

$same_key->quit;
wait_for_pid_to_leave_pg_stat_activity($same_key_pid,
	'same-key GUC shell session exits');
wait_for_validation_ok_for_pid(
	$same_key_pid,
	'shell validation succeeds after same-key GUC reset proof');

is($node->safe_psql(
		'phase16b_guc_db',
		q{
SELECT current_setting('work_mem') || ',' ||
       current_setting('statement_timeout') || ',' ||
       current_setting('default_statistics_target') || ',' ||
       current_setting('lock_timeout') || ',' ||
       test_backend_runtime_guc_reset_baseline_matches();
},
		connstr => $same_key_connstr),
	'3MB,7s,77,8s,true',
	'next same-key client starts from role/database/startup GUC baseline');

is($node->safe_psql(
		'phase16b_guc_db',
		q{
SELECT current_setting('work_mem') || ',' ||
       current_setting('statement_timeout') || ',' ||
       current_setting('default_statistics_target') || ',' ||
       current_setting('lock_timeout') || ',' ||
       test_backend_runtime_guc_reset_baseline_matches();
},
		connstr => $other_startup_connstr),
	'3MB,7s,77,11s,true',
	'different startup option has its own reset baseline');

unlike(
	slurp_file($node->logfile),
	qr/PANIC|segmentation|server closed the connection unexpectedly|server process .* was terminated|was terminated by signal/,
	'same-key GUC baseline log has no crash signatures');

$node->stop('fast');

done_testing();
