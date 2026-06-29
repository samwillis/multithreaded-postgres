# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time);

if (!$use_unix_sockets)
{
	plan skip_all =>
	  "authentication tests cannot run without Unix-domain sockets";
}

my $delay_ms = 1000;
my $minimum_elapsed_ms = 800;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	qq{
shared_preload_libraries = 'auth_delay'
auth_delay.milliseconds = $delay_ms
});
$node->start;

$node->safe_psql('postgres',
	"CREATE ROLE auth_delay_user LOGIN PASSWORD 'correct_password'");

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	"local all auth_delay_user password");
$node->reload;

local $ENV{PGPASSWORD} = 'wrong_password';
my $start_time = time();

$node->connect_fails(
	'user=auth_delay_user',
	'auth_delay applies to failed authentication',
	expected_stderr => qr/password authentication failed for user "auth_delay_user"/);

my $elapsed_ms = (time() - $start_time) * 1000;
cmp_ok(
	$elapsed_ms,
	'>=',
	$minimum_elapsed_ms,
	"failed authentication was delayed by auth_delay");

done_testing();
