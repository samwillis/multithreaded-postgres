#!/usr/bin/env perl

use strict;
use warnings FATAL => 'all';
use Getopt::Long;
use File::Spec;

my $top_srcdir = '.';
my $top_builddir = '.';
my $covered_file = 'plan_docs/MULTITHREADED_PHASE16_COVERED_COMPONENTS.tsv';
my $exclusions_file = 'plan_docs/MULTITHREADED_PHASE16_EXCLUSIONS.tsv';
my $list_components = 0;

GetOptions(
	'top-srcdir=s' => \$top_srcdir,
	'top-builddir=s' => \$top_builddir,
	'covered=s' => \$covered_file,
	'exclusions=s' => \$exclusions_file,
	'list-components' => \$list_components,
) or die "bad options\n";

my %make_vars = read_make_vars(File::Spec->catfile($top_builddir, 'src', 'Makefile.global'));

my @roots = check_world_roots();
my @components = sort map { derive_leaf_components($_) } @roots;
my %component = map { $_ => 1 } @components;
my %target_graph = read_make_target_graph(File::Spec->catfile($top_srcdir, 'GNUmakefile.in'));
my %world_threaded_targets = reachable_targets('check-world-threaded', \%target_graph);

if ($list_components)
{
	print "$_\n" for @components;
	exit 0;
}

my %covered = read_covered_components($covered_file);
my %excluded = read_exclusions($exclusions_file);

my @errors;

for my $name (sort keys %covered)
{
	push @errors, "covered component also has exclusion row: $name"
		if exists $excluded{$name};
	push @errors, "covered component is not an enabled check-world leaf: $name"
		unless $component{$name};
	push @errors, "covered component has empty threaded target: $name"
		unless length $covered{$name}->{threaded_target};

	for my $target (threaded_targets($covered{$name}->{threaded_target}))
	{
		push @errors, "covered component target has invalid name: $name -> $target"
			unless plain_make_target($target);
		push @errors, "covered component target is not defined in GNUmakefile.in: $name -> $target"
			unless exists $target_graph{$target};
		push @errors, "covered component target is not reachable from check-world-threaded: $name -> $target"
			if exists $target_graph{$target}
			&& !$world_threaded_targets{$target};
	}
}

for my $name (sort keys %excluded)
{
	my $row = $excluded{$name};
	my $status = $row->{status};

	push @errors, "exclusion component path does not exist: $name"
		unless -e File::Spec->catfile($top_srcdir, split('/', $name));
	push @errors, "exclusion row for enabled component should not be configure_disabled: $name"
		if $component{$name} && $status eq 'configure_disabled';
	push @errors, "non-configure-disabled exclusion is not an enabled check-world leaf: $name"
		if !$component{$name} && $status ne 'configure_disabled';
	push @errors, "exclusion row missing reason: $name"
		unless length $row->{reason};
	push @errors, "temporary blocker must set release_blocker to yes or no: $name"
		if $status eq 'temporarily_blocked'
		&& $row->{release_blocker} !~ /^(?:yes|no)$/;
	push @errors, "configure_disabled row must name configure condition: $name"
		if $status eq 'configure_disabled'
		&& $row->{reason} !~ /(?:\bwith_[A-Za-z0-9_]*|\benable_[A-Za-z0-9_]*|\bPORTNAME\b|\bconfigured\b|\bconfigure\b)/;
}

for my $name (@components)
{
	next if exists $covered{$name};
	next if exists $excluded{$name};
	push @errors, "enabled check-world leaf has no threaded coverage or exclusion: $name";
}

if (@errors)
{
	print "check-threaded-world-coverage: FAILED\n";
	print "  $_\n" for @errors;
	exit 1;
}

printf "check-threaded-world-coverage: ok (%d covered, %d excluded, %d enabled leaves)\n",
	scalar(keys %covered), scalar(keys %excluded), scalar(@components);

exit 0;

sub check_world_roots
{
	my $path = File::Spec->catfile($top_srcdir, 'GNUmakefile.in');
	open my $fh, '<', $path or die "could not read $path: $!\n";

	while (my $line = <$fh>)
	{
		if ($line =~ /\$\(call\s+recurse,\s*check-world,\s*([^,]+),\s*check\)/)
		{
			my @dirs = split /\s+/, trim($1);
			die "no check-world roots found in $path\n" unless @dirs;
			return @dirs;
		}
	}

	die "could not find check-world recurse rule in $path\n";
}

sub derive_leaf_components
{
	my ($component) = @_;
	my $dir = File::Spec->catdir($top_srcdir, split('/', $component));
	my $makefile = File::Spec->catfile($dir, 'Makefile');

	return ($component) unless -f $makefile;

	my @subdirs = active_subdirs($makefile);
	return ($component) unless @subdirs;

	my @leaves;
	for my $subdir (@subdirs)
	{
		next if $subdir =~ /\$\(/;
		push @leaves, derive_leaf_components("$component/$subdir");
	}
	return @leaves ? @leaves : ($component);
}

sub active_subdirs
{
	my ($makefile) = @_;
	my @lines = logical_makefile_lines($makefile);
	my @subdirs;
	my @frames;
	my $active = 1;

	for my $line (@lines)
	{
		$line =~ s/#.*$//;
		$line = trim($line);
		next unless length $line;

		if ($line =~ /^ifeq\s*\((.*),(.*)\)$/)
		{
			my $cond = expand_make_value(trim($1)) eq expand_make_value(trim($2));
			push @frames, { parent => $active, cond => $cond };
			$active = $active && $cond;
			next;
		}
		if ($line =~ /^ifneq\s*\((.*),(.*)\)$/)
		{
			my $cond = expand_make_value(trim($1)) ne expand_make_value(trim($2));
			push @frames, { parent => $active, cond => $cond };
			$active = $active && $cond;
			next;
		}
		if ($line =~ /^ifdef\s+(.+)$/)
		{
			my $cond = length expand_make_value('$(' . trim($1) . ')');
			push @frames, { parent => $active, cond => $cond };
			$active = $active && $cond;
			next;
		}
		if ($line =~ /^ifndef\s+(.+)$/)
		{
			my $cond = !length expand_make_value('$(' . trim($1) . ')');
			push @frames, { parent => $active, cond => $cond };
			$active = $active && $cond;
			next;
		}
		if ($line =~ /^else\b/)
		{
			die "else without if in $makefile\n" unless @frames;
			$frames[-1]->{cond} = !$frames[-1]->{cond};
			$active = $frames[-1]->{parent} && $frames[-1]->{cond};
			next;
		}
		if ($line =~ /^endif\b/)
		{
			die "endif without if in $makefile\n" unless @frames;
			pop @frames;
			$active = @frames ? ($frames[-1]->{parent} && $frames[-1]->{cond}) : 1;
			next;
		}

		next unless $active;

		if ($line =~ /^SUBDIRS\s*([+:]?=)\s*(.*)$/)
		{
			my ($op, $value) = ($1, $2);
			my @items = grep { length && $_ !~ /\$\(/ } split /\s+/, trim($value);
			@subdirs = () if $op ne '+=';
			push @subdirs, @items;
		}
	}

	die "unterminated conditional in $makefile\n" if @frames;
	return @subdirs;
}

sub logical_makefile_lines
{
	my ($path) = @_;
	open my $fh, '<', $path or die "could not read $path: $!\n";

	my @lines;
	my $current = '';
	while (my $line = <$fh>)
	{
		chomp $line;
		if ($line =~ s/\\$//)
		{
			$current .= $line . ' ';
			next;
		}
		push @lines, $current . $line;
		$current = '';
	}
	push @lines, $current if length $current;
	return @lines;
}

sub read_make_vars
{
	my ($path) = @_;
	my %vars;
	return %vars unless -f $path;

	open my $fh, '<', $path or die "could not read $path: $!\n";
	while (my $line = <$fh>)
	{
		chomp $line;
		next unless $line =~ /^([A-Za-z0-9_]+)\s*[:?]?=\s*(.*)$/;
		$vars{$1} = trim($2);
	}
	return %vars;
}

sub expand_make_value
{
	my ($value) = @_;
	$value =~ s/^\s*["']//;
	$value =~ s/["']\s*$//;
	$value =~ s/\$\(([^)]+)\)/exists $make_vars{$1} ? $make_vars{$1} : ''/ge;
	return trim($value);
}

sub read_make_target_graph
{
	my ($path) = @_;
	my %graph;
	my @current_targets;

	for my $raw (logical_makefile_lines($path))
	{
		if ($raw =~ /^\s/)
		{
			while (@current_targets
				&& $raw =~ /\$\((?:MAKE|GMAKE)\)\s+([A-Za-z0-9_.-]+)/g)
			{
				my $called = $1;
				next unless plain_make_target($called);
				push @{$graph{$_}}, $called for @current_targets;
			}
			next;
		}

		@current_targets = ();
		$raw =~ s/#.*$//;
		my $line = trim($raw);
		next unless length $line;
		next if $line =~ /^(?:if(?:eq|neq|def|ndef)|else|endif)\b/;
		next if $line =~ /^[A-Za-z0-9_]+\s*[:?+]?=/;
		next unless $line =~ /^([^:=]+?)\s*:(.*)$/;

		my ($lhs, $rhs) = ($1, $2);
		my @targets = grep { plain_make_target($_) } split /\s+/, trim($lhs);
		next unless @targets;

		$graph{$_} ||= [] for @targets;
		@current_targets = @targets;

		$rhs =~ s/\|/ /g;
		my @deps = grep {
			plain_make_target($_) && $_ !~ /=/
		} split /\s+/, trim($rhs);
		push @{$graph{$_}}, @deps for @targets;
	}

	return %graph;
}

sub reachable_targets
{
	my ($root, $graph) = @_;
	my %seen;
	my @queue = ($root);

	while (@queue)
	{
		my $target = shift @queue;
		next if $seen{$target}++;
		push @queue, grep { !$seen{$_} } @{$graph->{$target} || []};
	}

	return %seen;
}

sub threaded_targets
{
	my ($value) = @_;
	return grep { length } split /\s+/, trim($value);
}

sub plain_make_target
{
	my ($value) = @_;
	return defined $value && $value =~ /^[A-Za-z0-9][A-Za-z0-9_.-]*$/;
}

sub read_covered_components
{
	my ($path) = @_;
	my %rows;
	read_tsv(
		$path,
		[qw(component threaded_target notes)],
		sub {
			my ($row) = @_;
			die "duplicate covered component $row->{component}\n"
				if exists $rows{$row->{component}};
			$rows{$row->{component}} = $row;
		});
	return %rows;
}

sub read_exclusions
{
	my ($path) = @_;
	my %valid_type = map { $_ => 1 } qw(contrib pl src-test test-module bin interface tool isolation tap other);
	my %valid_status = map { $_ => 1 } qw(temporarily_blocked process_only_by_design release_blocker configure_disabled not_applicable);
	my %rows;

	read_tsv(
		$path,
		[qw(component type status release_blocker reason replacement_guard owner_notes)],
		sub {
			my ($row) = @_;
			die "duplicate exclusion component $row->{component}\n"
				if exists $rows{$row->{component}};
			die "invalid exclusion type $row->{type} for $row->{component}\n"
				unless $valid_type{$row->{type}};
			die "invalid exclusion status $row->{status} for $row->{component}\n"
				unless $valid_status{$row->{status}};
			$rows{$row->{component}} = $row;
		});
	return %rows;
}

sub read_tsv
{
	my ($path, $columns, $callback) = @_;
	open my $fh, '<', $path or die "could not read $path: $!\n";

	while (my $line = <$fh>)
	{
		chomp $line;
		next if $line =~ /^\s*(?:#.*)?$/;
		my @values = split /\t/, $line, scalar(@$columns);
		die "wrong column count in $path line $.: $line\n"
			unless @values == @$columns;
		my %row;
		@row{@$columns} = @values;
		$row{component} = trim($row{component});
		$callback->(\%row);
	}
}

sub trim
{
	my ($value) = @_;
	$value = '' unless defined $value;
	$value =~ s/^\s+//;
	$value =~ s/\s+$//;
	return $value;
}
