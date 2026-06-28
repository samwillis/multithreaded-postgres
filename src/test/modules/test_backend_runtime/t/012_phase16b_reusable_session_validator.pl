# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use Test::More;

my $node =
  PostgreSQL::Test::Cluster->new('phase16b_reusable_session_validator');

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
	'Phase 16B validator TAP starts threaded runtime');

$node->safe_psql('postgres', 'CREATE EXTENSION test_backend_runtime_threaded;');
is($node->safe_psql(
		'postgres',
		'SELECT test_backend_runtime_reusable_session_validator_reasons();'),
	't',
	'reusable-session validator rejects initial dirty reason set');

my $guc = $node->background_psql('postgres', timeout => 30);
is($guc->query_safe(
		q{SELECT test_backend_runtime_guc_reset_baseline_matches();},
		verbose => 0),
	't',
	'GUC reset baseline initially matches in threaded runtime');
$guc->query_safe(q{SET work_mem = '64MB';}, verbose => 0);
is($guc->query_safe(
		q{SELECT test_backend_runtime_guc_reset_baseline_matches();},
		verbose => 0),
	'f',
	'GUC reset baseline detects session SET drift');
$guc->query_safe(q{RESET work_mem;}, verbose => 0);
is($guc->query_safe(
		q{SELECT test_backend_runtime_guc_reset_baseline_matches();},
		verbose => 0),
	't',
	'GUC reset baseline matches again after RESET');
$guc->quit;

$node->stop('fast');

done_testing();
