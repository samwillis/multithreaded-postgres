# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('phase16b_shell_pool_fallback');

sub wait_for_pids_to_leave_pg_stat_activity
{
	my ($pids, $label) = @_;
	my $pid_list = join(',', map { int($_) } @$pids);

	$node->poll_query_until(
		'postgres',
		"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid IN ($pid_list));",
		't') || die "timed out waiting for $label";
	pass($label);
}

sub wait_for_shell_pool_fallback_stats
{
	my ($label) = @_;
	my $log = '';

	for (1 .. 100)
	{
		$log = slurp_file($node->logfile);
		foreach my $line (split /\n/, $log)
		{
			next unless $line =~ /threaded_session_pool_stats/;
			next unless $line =~ /start_failures=0/;
			next unless $line =~ /carrier_starts=(\d+)/;
			my $carrier_starts = $1;
			next unless $line =~ /carrier_limit_fallbacks=([1-9]\d*)/;
			my $fallbacks = $1;

			ok($carrier_starts <= 2,
				'shell carrier starts remain within threaded_session_pool_max');
			ok($fallbacks > 0,
				'shell pool records dedicated fallback starts');
			pass($label);
			return;
		}
		usleep(100_000);
	}

	fail($label);
	diag("server log did not contain shell fallback stats:\n$log");
}

$node->init;
$node->append_conf(
	'postgresql.conf', q{
multithreaded = on
threaded_session_pool = shell
threaded_session_pool_max = 2
pooled_protocol_carriers = 0
log_threaded_lifecycle_timing = on
autovacuum = off
io_method = sync
summarize_wal = off
log_min_messages = debug1
});
$node->start;

is($node->safe_psql('postgres', 'SHOW multithreaded'), 'on',
	'shell pool fallback test starts threaded runtime');
is($node->safe_psql('postgres', 'SHOW threaded_session_pool'), 'shell',
	'shell pool mode is configured');
is($node->safe_psql('postgres', 'SHOW threaded_session_pool_max'), '2',
	'shell pool carrier cache limit is configured');
is($node->safe_psql('postgres', 'SHOW pooled_protocol_carriers'), '0',
	'shell pool test does not enable pooled protocol mode');

# Seed the shell pool and give the warmed carrier time to become idle.
is($node->safe_psql('postgres', 'SELECT 16011;'), '16011',
	'shell pool server accepts an initial connection');
usleep(200_000);
is($node->safe_psql('postgres', 'SELECT 16012;'), '16012',
	'shell pool serves a short connection after warmup');
usleep(200_000);

my @sessions;
my @pids;
for my $i (1 .. 5)
{
	my $session = $node->background_psql('postgres', timeout => 30);
	my $pid = $session->query_safe('SELECT pg_backend_pid();', verbose => 0);

	push @sessions, $session;
	push @pids, $pid;

	is($session->query_safe("SELECT 16020 + $i;", verbose => 0),
		16020 + $i,
		"shell pool concurrent session $i remains queryable");
}

my $pid_list = join(',', map { int($_) } @pids);
is($node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_stat_activity WHERE pid IN ($pid_list);"),
	'5',
	'more clients than shell carriers are admitted concurrently');

for my $session (@sessions)
{
	$session->quit;
}
wait_for_pids_to_leave_pg_stat_activity(\@pids,
	'shell pool concurrent sessions exit');

wait_for_shell_pool_fallback_stats(
	'shell pool uses dedicated fallback instead of admission queueing');

unlike(
	slurp_file($node->logfile),
	qr/PANIC|segmentation|server closed the connection unexpectedly|server process .* was terminated|was terminated by signal/,
	'shell pool fallback log has no crash signatures');

$node->stop('fast');

done_testing();
