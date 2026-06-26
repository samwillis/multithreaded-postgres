# Phase 16 Plan: Threaded World And Extension Hardening

Phase 16 closes Gate E2-Extensions / Gate G after the Phase 15 pooled
protocol scheduler is complete. The goal is to make threaded mode credible for
the whole bundled PostgreSQL tree: contrib extensions, bundled procedural
languages, core and auxiliary test suites, client and interface tests,
extension GUCs, hooks, tools, and failure paths.

The phase should be driven by a strict coverage rule:

```text
check-world-threaded = check-world - explicit_manifest_exclusions
```

Every component covered by `check-world` must either run under
`check-world-threaded` or appear in a checked manifest with a concrete reason
and status. There should be no hidden skips, implicit omissions, or target names
that sound broader than the work they actually perform.

## Scope

Phase 16 owns:

- `check-world-threaded` coverage for every `check-world` test component that
  has a meaningful threaded-mode equivalent;
- contrib-wide threaded regression coverage;
- bundled procedural languages beyond PL/pgSQL;
- non-contrib `check-world` coverage under `src/test`, `src/pl`,
  `src/interfaces`, `src/bin`, and `src/tools/pg_bsd_indent`;
- custom and extension GUC ownership, hook, and replay semantics;
- extension backend-model metadata and compatibility levels;
- extension-owned and module-owned runtime/session APIs needed by in-tree
  modules;
- threaded stress coverage for extension load, unload, cancellation, teardown,
  waits, interrupts, `ERROR`, `FATAL`, and postmaster shutdown behavior;
- lock-order documentation for new runtime locks;
- debug views for runtime, backend, session, and carrier state where they are
  needed to validate Phase 16 hardening;
- sanitizer and performance baselines after Phase 15.

Phase 16 does not own:

- arbitrary third-party extension compatibility;
- Phase 17 deep scheduler-yielding boundaries;
- frontend output yielding, COPY continuations, lock-wait yielding, executor
  continuations, or task reentrancy;
- weakening process-mode `check-world` behavior;
- claiming pooled migratability for modules that are only thread-per-session
  safe.

## Compatibility Vocabulary

Every bundled module admitted by Phase 16 should be classified explicitly.

- `process-only`: rejected from threaded mode with a documented reason.
- `thread-per-session`: safe when each logical backend has a dedicated carrier.
- `pooled-protocol-affine`: safe in pooled protocol mode only when the session
  resumes on its original carrier or when the scheduler preserves affinity.
- `pooled-protocol-migratable`: safe for Phase 15 carrier migration at top-level
  protocol boundaries.
- `task-reentrant`: out of scope for Phase 16 and reserved for later Phase 17
  or deeper scheduler work.

Do not use a single coarse "pooled scheduler safe" marker. Phase 15 makes the
affine-versus-migratable distinction observable, and loader metadata should not
admit modules into a stronger runtime model than they have proven.

## Required Artifacts

Phase 16 should add these artifacts early:

- `check-world-threaded`: the broad threaded-world validation target.
- `check-threaded-contrib`: threaded contrib regression target.
- `check-threaded-pl`: threaded procedural-language target.
- `check-threaded-src-test`: threaded `src/test` orchestration target.
- `check-threaded-test-modules`: threaded `src/test/modules` target, called by
  `check-threaded-src-test` and available directly for focused runs.
- `check-threaded-interfaces`: threaded interface/tool coverage where relevant
  to `check-world`.
- `check-threaded-bin`: threaded `src/bin` coverage target.
- `check-threaded-tools`: threaded `src/tools/pg_bsd_indent` coverage target.
- `check-threaded-world-coverage`: verifier for the strict coverage contract.
- `plan_docs/MULTITHREADED_PHASE16_COVERED_COMPONENTS.tsv`: checked list of
  currently covered threaded-world leaves and their threaded targets.
- `plan_docs/MULTITHREADED_PHASE16_EXCLUSIONS.tsv`: checked manifest of
  temporary or permanent exclusions.
- A generated or maintained inventory for all `check-world` components,
  including contrib, PL, test, interface, bin, and tool components.

The first version of `check-world-threaded` may expose many blockers. That is
acceptable only if the coverage verifier is strict and every omission is named.

## Main-Plan Work Items

The detailed Phase 16 plan carries forward every work item from the main plan.

- Migrate every contrib extension to explicit backend model metadata.
- Make every contrib extension support thread-per-session mode by default, or
  list it as an explicit manifest exception.
- Complete bundled procedural-language support beyond PL/pgSQL, or explicitly
  mark any temporary exception as process-only with a release-blocking note.
- Finish custom and extension GUC ownership and hook semantics for threaded
  mode.
- Add session/runtime APIs needed by in-tree modules that currently rely on
  process-global mutable state.
- Run contrib regression tests in process mode and threaded mode.
- Document any temporary exception as a release-blocking gap rather than an
  unknown default.
- Run thread sanitizer where feasible.
- Run address sanitizer where feasible.
- Add stress tests for interrupts, waits, cancellation, and teardown.
- Document lock ordering for new runtime locks.
- Add debug views for runtime, backend, session, and carrier state where they
  are needed to validate or debug threaded-world hardening.
- Add crash and `FATAL` behavior tests.
- Record performance baselines.

## `check-world` Coverage Surface

The current top-level `check-world` rule recurses into:

```text
src/test
src/pl
src/interfaces
contrib
src/bin
src/tools/pg_bsd_indent
```

`check-world-threaded` must cover all of those categories, not just contrib.
The coverage verifier should derive or check this list from the build system so
future changes to `check-world` cannot silently escape threaded coverage.

The canonical coverage unit is a leaf recursive `check-world` component, not
only a top-level directory. The verifier should identify leaf units such as
individual `src/bin/*`, `src/interfaces/*`, `src/pl/*`, `src/test/*`,
`contrib/*`, and `src/tools/pg_bsd_indent` check targets, including their
configure condition. A parent directory can be marked covered only if every
enabled leaf under that parent is covered by a threaded target or listed in the
manifest.

Non-contrib coverage categories:

- `src/test`: core regression, isolation, modules, Perl support tests,
  postmaster tests, recovery tests, subscription tests, authentication tests,
  and optional ICU, Kerberos, LDAP, and SSL tests when enabled by configure.
- `src/pl`: PL/pgSQL, PL/Perl, PL/Python, and PL/Tcl according to configure
  support.
- `src/interfaces`: libpq, ecpg, and libpq-oauth when configured.
- `src/bin`: initdb, pg_amcheck, pg_archivecleanup, pg_basebackup,
  pg_checksums, pg_combinebackup, pg_config, pg_controldata, pg_ctl, pg_dump,
  pg_resetwal, pg_rewind, pg_test_fsync, pg_test_timing, pg_upgrade,
  pg_verifybackup, pg_waldump, pg_walsummary, pgbench, psql, scripts, and
  pgevent where applicable.
- `src/tools/pg_bsd_indent`: pg_bsd_indent TAP/test coverage.

`world` and `install-world` also include build/install surfaces such as `doc`
and `config`. Those are not `check-world` test components, but if Phase 16
adds build-oriented threaded-world or install-world variants, they should be
listed as build-only coverage or `not_applicable` manifest rows rather than
being ignored.

## Exclusion Manifest

The manifest should be machine-checked. A TSV format is sufficient and keeps
review noise low.

Suggested columns:

```text
component	type	status	release_blocker	reason	replacement_guard	owner_notes
```

Suggested `type` values:

- `contrib`
- `pl`
- `src-test`
- `test-module`
- `bin`
- `interface`
- `tool`
- `isolation`
- `tap`
- `other`

Suggested `status` values:

- `temporarily_blocked`
- `process_only_by_design`
- `release_blocker`
- `configure_disabled`
- `not_applicable`

Rules:

- Every skipped `check-world` component must have one manifest row.
- Every row must name a concrete technical reason.
- Every temporary blocker must say whether it blocks release.
- A `process_only_by_design` row must explain why threaded mode should never
  admit that component.
- A `not_applicable` row must explain why the `check-world` component has no
  meaningful threaded equivalent.
- A `configure_disabled` row must identify the configure option or dependency
  that excluded the component from the current build.
- The verifier must fail on stale rows for removed components.
- The verifier must fail when a `check-world` component is neither covered by
  `check-world-threaded` nor listed in the manifest.
- If an expected-fail component starts passing, the manifest must be updated in
  the same commit that admits it.

## Phase Entry Preconditions

Before Phase 16 implementation starts:

1. Record the Phase 15 HEAD and Gate F validation result.
2. Confirm process mode remains green.
3. Confirm thread-per-session and pooled protocol scheduler validation remains
   green.
4. Capture performance baselines for process, thread-per-session, and pooled
   protocol modes.
5. Record current known extension and PL exclusions inherited from Gate
   E2-Core.

Minimum expected baseline:

```text
gmake check-world
gmake check
gmake check-threaded
gmake check-threaded-workers
gmake check-threaded-world-core
gmake check-runtime-lifecycles
gmake check-global-lifetimes
git diff --check
```

Phase 16 work may add more specific targets, but should not reduce this floor
when touching runtime ownership, extension state, GUCs, hooks, workers, or
failure paths.

If local platform or optional-dependency friction makes literal `gmake
check-world` noisy, record the near-equivalent process-mode world baseline and
the exact manifest of missing process-world components before starting threaded
world work. Do not mix pre-existing process-world failures with
threaded-world regressions.

## Step 1: Define Threaded World Coverage

Add the formal coverage rule to the plan and build system:

```text
check-world-threaded = check-world - explicit_manifest_exclusions
```

Implementation direction:

- identify the components that `check-world` runs in this tree;
- define the threaded equivalent target list;
- record currently covered leaves in
  `plan_docs/MULTITHREADED_PHASE16_COVERED_COMPONENTS.tsv`;
- add the exclusion manifest;
- add a verifier that compares all three lists;
- make `check-world-threaded` run the verifier.

The first useful target is allowed to fail because modules are not ready. It is
not allowed to omit modules silently.

## Step 2: Build The Coverage Verifier

The verifier should fail on:

- a `check-world` component missing from both threaded coverage and manifest;
- a manifest row for a nonexistent component;
- a manifest row without a concrete reason;
- a temporary blocker without `release_blocker` classification;
- duplicate or ambiguous component names;
- a threaded target entry that does not map back to a known `check-world`
  component;
- a passing component that remains listed as blocked.

Prefer a deterministic text report suitable for review:

```text
covered: contrib/citext
excluded: src/test/ssl configure_disabled "with_ssl=no in this build"
missing: contrib/example_module
stale: contrib/removed_module
```

## Step 3: Add Initial `check-world-threaded`

Start with a target shape like:

```make
check-world-threaded:
	$(MAKE) check-threaded-world-core
	$(MAKE) check-threaded-src-test
	$(MAKE) check-threaded-contrib
	$(MAKE) check-threaded-pl
	$(MAKE) check-threaded-interfaces
	$(MAKE) check-threaded-bin
	$(MAKE) check-threaded-tools
	$(MAKE) check-threaded-world-coverage
```

Keep target names specific. If a subtarget does not yet cover all components in
its category, its manifest and verifier must make that explicit.

`check-threaded-src-test` should orchestrate the `src/test` leaf categories,
including `check-threaded-test-modules`. Keep `check-threaded-test-modules` as a
focused target, but do not require top-level `check-world-threaded` to call both
unless the duplicate coverage is intentional and documented.

## Step 4: Generate The World Inventory

Create an inventory for each `check-world` component. It can start as TSV or
generated markdown, but it must be precise enough to drive implementation
order.

Track at least:

- component path;
- build target and test target;
- `check-world` category;
- threaded target or manifest row;
- configure condition, if any;
- current backend model metadata;
- current Phase 16 compatibility classification;
- `PG_MODULE_MAGIC_EXT` status;
- `_PG_init()` and `_PG_fini()` use;
- custom GUC definitions;
- assign/check/show hooks;
- shared memory use;
- background worker registration;
- hooks installed into backend subsystems;
- mutable file-scope state;
- external libraries or interpreter state;
- existing process-mode regression/TAP/isolation coverage;
- threaded coverage status;
- pooled-affinity or migration limitations.

For non-extension components, track the threaded-mode relevance explicitly.
Some `src/bin`, `src/interfaces`, and tool tests are client-side and may not
need special threaded runtime state, but they still need coverage or a manifest
classification because they are part of `check-world`.

This inventory should decide the order of work. Avoid picking modules or test
families manually once the inventory can identify lower-risk tranches.

## Step 5: Map Non-Contrib `check-world` Tests

Bring the non-contrib `check-world` categories under the same coverage rule
before broad contrib admission. This step may initially populate the manifest
instead of making every category pass, but it must make every leaf component
visible.

Required `src/test` coverage:

- `src/test/regress`: already covered by the core threaded regression target,
  but the coverage verifier should still map the `check-world` component to
  that threaded target.
- `src/test/isolation`: already covered by `check-threaded-world-core`; keep it
  mapped explicitly.
- `src/test/modules`: split into admitted threaded modules, process-only
  modules, and manifest exclusions.
- `src/test/perl`: classify whether its tests are support-only, client-side,
  or meaningful threaded runtime checks.
- `src/test/postmaster`: cover postmaster death, shutdown, and control-plane
  behavior relevant to threaded and pooled modes.
- `src/test/recovery`: cover recovery, replication, restart, and failover tests
  that interact with threaded workers or threaded backend sessions.
- `src/test/subscription`: cover logical replication and subscription tests,
  including worker behavior and teardown.
- `src/test/authentication`: cover authentication and connection-startup tests
  where threaded mode changes backend startup or module loading.
- optional `src/test/icu`, `src/test/kerberos`, `src/test/ldap`, and
  `src/test/ssl`: cover when configured; otherwise classify as
  `configure_disabled`, not as a threaded skip.

Required `src/pl` coverage:

- PL/pgSQL remains a required core language and should stay covered.
- PL/Perl, PL/Python, and PL/Tcl need threaded checks or justified manifest
  rows. PL/Python and PL/Tcl now have dependency-enabled scratch threaded
  evidence, but remain `configure_disabled` manifest rows in this default
  `with_python=no`/`with_tcl=no` build so the verifier's enabled-leaf contract
  stays exact.
- Contrib language adapters such as `hstore_plperl`, `jsonb_plpython`, and
  related modules should be tied to the corresponding procedural-language
  classification.

Required `src/interfaces` coverage:

- libpq tests must cover threaded and pooled server interactions where backend
  startup, authentication, cancellation, pipeline behavior, SSL/GSS transport,
  or postmaster death semantics are relevant.
- ecpg tests must either run against threaded mode or be explicitly classified
  as client-side coverage with a threaded server replacement guard.
- libpq-oauth must be included when configured and classified as
  `configure_disabled` when not configured.

Required `src/bin` coverage:

- utilities that start, stop, initialize, upgrade, dump, restore, replicate, or
  benchmark servers must run against an appropriate threaded server where that
  changes behavior.
- client-only or file-format-only tests may be classified as unchanged by
  threaded mode, but the manifest or inventory must say that explicitly.
- pgbench coverage should include process, thread-per-session, and pooled
  protocol baselines because it is both a `check-world` component and the
  primary performance harness.

Required `src/tools/pg_bsd_indent` coverage:

- keep the existing tool test in `check-world-threaded`, or classify it as
  build/tool-only with no threaded-server dependency. Do not let it disappear
  from the coverage verifier.

## Step 6: Finish Extension GUC Semantics Early

Custom and extension GUC behavior is cross-cutting. Complete the reusable
semantics before trying to admit many modules.

Required coverage:

- `_PG_init()` replay and adoption for already-loaded modules;
- concurrent sessions loading the same extension;
- custom check, assign, and show hooks;
- `SET`, `SET LOCAL`, `RESET`, transaction abort, subtransaction abort, and
  session reset;
- database defaults, role defaults, and startup packet options;
- reload behavior;
- extension GUC behavior after `ERROR`, `FATAL`, terminate, and reconnect;
- process-mode behavior unchanged.

Expected deliverable:

- focused threaded custom-GUC stress coverage in `src/test/modules` or an
  equivalent in-tree test module;
- documentation of the remaining temporary process-wide critical section, or
  removal of that critical section if Phase 16 completes the semantics.

## Step 7: Admit Low-Risk Contrib Modules

Admit simple modules first to validate the harness and metadata flow.

Likely early candidates are modules that are mostly SQL/C functions, data
types, operators, or inspection helpers, such as:

- `citext`
- `cube`
- `earthdistance`
- `fuzzystrmatch`
- `intagg`
- `intarray`
- `ltree`
- `pg_freespacemap`
- `pg_visibility`
- `pg_walinspect`
- `pgstattuple`
- `seg`
- `tablefunc`
- `unaccent`

For each admitted module:

1. Add or verify explicit backend-model metadata.
2. Run process-mode module tests.
3. Run threaded module tests.
4. Check mutable globals and owner mappings.
5. Update the inventory.
6. Remove its exclusion-manifest row in the same commit.

Do not batch modules so broadly that a failure cannot be assigned to a module
or shared mechanism.

## Step 8: Admit Hook And GUC Extensions

Next handle modules whose risk is mostly hooks, GUCs, or per-session policy.

Likely candidates:

- `auth_delay`
- `auto_explain`
- `basebackup_to_shell`
- `passwordcheck`
- `pg_overexplain`
- `pg_plan_advice`
- `pg_trgm`

Focus areas:

- hook install order;
- hook chaining and uninstall expectations;
- GUC assign/check/show hook state;
- session-local versus runtime-global state;
- teardown after `ERROR`, `FATAL`, cancel, terminate, and reconnect;
- process-mode hook behavior unchanged.

Add lock-order notes for any new runtime lock introduced to support hook or GUC
state.

`basic_archive` is admitted in this tranche. Its threaded coverage depends on
serializing dynamic library `_PG_init()` replay with `config_exec_params`
replay, so archive-module custom GUC prefix reservation is observed
consistently by auxiliary workers and client backends.

## Step 9: Admit Shared-Memory, Stats, Worker, And Logical Modules

This tranche should come after the GUC and simple-extension floor is stable.

Likely candidates:

- `pg_stat_statements`
- `pg_buffercache`
- `pg_prewarm`
- `pg_stash_advice`
- `test_decoding`
- logical inspection modules;
- modules with dynamic or static background worker paths.

Focus areas:

- shared memory initialization;
- LWLock use;
- process-global registry lifetime;
- worker startup, restart, shutdown, and crash escalation;
- logical decoding session and runtime state;
- backend termination while extension state is active;
- pooled protocol affinity limitations.

Do not admit a module into pooled-protocol-migratable mode just because it is
thread-per-session safe.

## Step 10: Admit External Runtime And Security Modules

Handle interpreter, security, and external-library modules last unless earlier
work proves they are simpler than expected.

Likely high-risk modules:

- `src/pl/plperl`
- `src/pl/plpython`
- `src/pl/tcl`
- `contrib/sepgsql`
- `contrib/uuid-ossp`
- `contrib/xml2`
- parts of `contrib/pgcrypto` if external library assumptions matter.

For procedural languages:

- audit interpreter-global state explicitly;
- distinguish trusted and untrusted variants;
- verify startup GUC behavior;
- verify repeated load and reconnect;
- verify `ERROR`, `FATAL`, cancel, and terminate cleanup;
- classify pooled affinity or migration separately from thread-per-session.

Current PL/Python admission evidence moves session globals, the execution stack,
and explicit subtransactions into `PgSession.extension_modules`, keeps the
shared `plpy` SPI exception hash private context under
`PgRuntime.extension_modules`, and brackets Python execution/reset paths with
the GIL. A Python-enabled scratch build under `/tmp/phase16-python-worktree`
passes the PL/Python and PL/Python transform threaded checks listed in the
Gate G evidence section.

Current PL/Tcl admission evidence keeps Tcl interpreter/procedure state in
`PgSession.extension_modules`, relies on dynamic-library `_PG_init()` replay for
per-session custom GUC and interpreter setup, and declares the module
thread-per-session after a Tcl-enabled scratch build under
`/tmp/phase16-tcl-worktree` passes the threaded PL/Tcl regression suite. Pooled
protocol migration remains a separate classification from thread-per-session
admission.

For security and external-library modules:

- document external thread-safety assumptions;
- synchronize or isolate process-global mutable state;
- retain a release-blocking manifest row until tests prove the assumption.

Current external-library module evidence comes from
`/tmp/phase16-optional-deps-src`, configured against locally extracted
dependency packages under `/tmp/phase16-debroot` with `--enable-tap-tests`,
`--with-ssl=openssl`, `--with-gssapi`, `--with-ldap`, `--with-selinux`,
`--with-uuid=e2fs`, `--with-libcurl`, `--with-libxml`, and `--with-libxslt`.
That scratch build passes threaded checks for `contrib/pgcrypto`,
`contrib/uuid-ossp`, `contrib/xml2`, SSL including `contrib/sslinfo`, ICU,
LDAP, Kerberos, `src/interfaces/libpq-oauth`,
`src/test/modules/ldap_password_func`, and
`src/test/modules/ssl_passphrase_callback`; exact target and log details are
recorded in the Gate G evidence section and the exclusions manifest.

`contrib/sepgsql` now declares thread-per-session module metadata and admits
threaded shared-preload replay without re-installing runtime-global hooks in
each session. A full sepgsql policy regression still needs an SELinux-enabled
host with file-context policy and the `sepgsql_regression_test_mode` boolean,
but the loader path is covered by a clean threaded postmaster boot with
`shared_preload_libraries='sepgsql'`.

## Step 11: Add Failure-Path Stress

Add extension- and module-focused failure coverage after enough components are
admitted for the tests to matter.

Coverage should include:

- extension load failure;
- `_PG_init()` failure;
- custom GUC hook failure;
- `ERROR` during extension execution;
- `FATAL` while extension state is active;
- query cancel while extension code is active;
- administrator termination while extension code is active;
- repeated connect, load, use, disconnect;
- postmaster shutdown with active threaded extension sessions;
- background worker crash and restart where applicable;
- retained-root and crash/corruption log guards.

These tests should feed `check-world-threaded` or a required subtarget. Avoid
leaving them as local scripts only.

Current failure-path stress is part of the required
`check-threaded-world-core-tap` bundle. `t/001_threaded_runtime.pl` covers
SQL `ERROR`, transaction abort, query cancel, backend terminate, backend
`FATAL`, abandoned clients, reconnect loops, extension load/drop, background
worker rejection/restart, and mixed teardown stress. Phase 16 adds
`t/010_phase16_extension_failure_paths.pl` to focus specifically on module
failure paths: missing module `LOAD`, repeated `_PG_init()` failure using a
test-only thread-compatible init-failure module, successful module load after
that failure, custom GUC stack recovery after extension `ERROR`, query cancel
while extension code is waiting, and fresh extension custom-GUC state after
extension `FATAL` and reconnect.

## Step 12: Add Debug Views And Lock Documentation

Add observability and documentation needed to make Phase 16 failures actionable.

Expected work:

- debug views for runtime, backend, session, carrier, extension-state, and
  module-admission state where existing views are insufficient;
- lock-order documentation for any new runtime, extension, hook, GUC, shared
  memory, worker, or interpreter locks;
- test output that ties failures back to component names in the world
  inventory and exclusion manifest;
- documented replacement guards for any manifest row that cannot run a direct
  threaded equivalent.

Keep debug views focused on validation and operations. Do not add broad
introspection tables unless they answer a concrete Phase 16 debugging need.

Current lock-order and debug-surface evidence is recorded in
`MULTITHREADED_PHASE16_LOCKS_AND_DEBUG.md`. The current documented Phase 16
cross-lock order is `DynamicFileManagerMutex -> ThreadedGUCMutex`; the current
debug decision is to rely on the coverage verifier, Phase 16 manifests,
lifecycle/global-lifetime checks, existing backend/session observability, and
component-named `check-world-threaded` output rather than adding a broad new
SQL introspection view.

## Step 13: Add Sanitizer Runs

Run sanitizers where feasible and document any platform-specific limitations.

Expected work:

- ASAN build and selected full suites;
- TSAN build if practical, with reviewed suppressions for known PostgreSQL or
  platform noise;
- repeated threaded-world runs under normal builds;
- targeted stress for cancellation, teardown, waits, and extension hooks.

Sanitizer results should be part of Gate G evidence, even if the exact target
set is smaller than ordinary `check-world-threaded`.

## Step 14: Record Performance Baselines

Phase 16 must not hide major regressions behind compatibility work.

Record:

- process-mode performance after Phase 16;
- thread-per-session performance after Phase 16;
- pooled protocol performance after Phase 16;
- comparison against the Phase 15 baseline;
- any known module-specific overhead from hooks, GUCs, or shared-memory
  synchronization.

If a broad extension mechanism adds measurable overhead, classify it as:

- process-mode overhead;
- thread-per-session overhead;
- pooled protocol overhead;
- benchmark noise.

Do not accept major unexplained regressions as a Gate G closeout condition.

## Current Gate G Evidence

This section records the current evidence gathered on the `phase16-plan`
branch. It is an evidence log, not by itself a declaration that Gate G is
closed.

Current branch evidence as of June 27, 2026:

- `check-threaded-world-coverage` passes with `141 covered`, `22 excluded`,
  and `142 enabled leaves`. The extra exclusion rows are checked
  `configure_disabled` rows for optional dependency leaves that are real
  `check-world` components but absent from this local configure, plus the
  existing `src/test/perl` support-module `not_applicable` row.
- `MALLOC_CHECK_=3 gmake check-phase16-gate-g-local` passes on the current
  Linux default configure after adding a top-level `temp-install` prerequisite
  to the local Gate G bundle. The evidence log is
  `/tmp/phase16-gate-g-local-fresh-temp-install.log`, with status `0`; it
  rebuilt `tmp_install`, ran process-mode `check-world`,
  `check-world-threaded`, `check-runtime-lifecycles`, and
  `check-global-lifetimes`.
- `MALLOC_CHECK_=3 gmake check-world-threaded` passes on the current Linux build
  after threaded DSM, DSA, plan-cache, GUC, dynamic-library, WAL-insert,
  wait-event teardown, WAL summarizer interrupt, basebackup target, extension
  GUC, sync-rep default, pg_upgrade prepared-transaction hardening, threaded
  latch-waitset runtime ordering, and dynamic-library session-init bookkeeping
  hardening, plus the optional-dependency module admission work recorded below.
  The current evidence log is
  `/tmp/phase16-check-world-threaded-after-optional-deps.log`, with status `0`.
- The current full threaded-world pass includes core regression, isolation,
  authentication, postmaster, recovery, subscription, `src/test/modules`,
  `src/pl`, contrib, interfaces, `src/bin`, and `src/tools/pg_bsd_indent`.
  Important non-contrib TAP leaves in that pass include recovery
  `Files=52, Tests=631`, subscription, `src/bin/pg_basebackup`, `src/bin/pg_combinebackup`
  (`Files=11, Tests=81`), `src/bin/pg_upgrade` (`Files=8, Tests=136`),
  `src/bin/pg_verifybackup`, `src/bin/pg_walsummary`, `src/bin/pgbench`,
  `src/bin/psql`, and `src/bin/scripts`.
- A threaded immediate-shutdown blocker found during `src/bin/pg_combinebackup`
  coverage was fixed by making the WAL summarizer honor `ProcDiePending` after
  consuming logical backend interrupts. The focused
  `check-threaded-bin-pg-combinebackup` leaf now passes before the full
  threaded-world pass and no longer leaves recalcitrant WAL summarizer
  postmasters behind.
- Focused Phase 16 failure-path stress now runs in
  `check-threaded-world-core-tap`, which is part of
  `check-threaded-test-backend-runtime` and `check-world-threaded`. The
  evidence log
  `/tmp/phase16-check-threaded-test-backend-runtime-failure-paths.log` shows
  `t/010_phase16_extension_failure_paths.pl` passing as part of
  `Files=10, Tests=297`; it covers missing module `LOAD`, repeated
  `_PG_init()` failure and recovery, extension `ERROR` with custom GUC rollback,
  cancel while extension code waits, extension `FATAL`, reconnect, and server
  usability after each failure path.
- process-mode `check-world` passes on the current Linux build. This configure
  has `enable_tap_tests = no`, `enable_injection_points = no`, `with_icu = no`,
  `with_python = no`, and `with_tcl = no`, so optional leaves not enabled by
  this build remain outside the current enabled-leaf count. The optional leaves
  are now still visible in
  `plan_docs/MULTITHREADED_PHASE16_EXCLUSIONS.tsv` as checked
  `configure_disabled` rows rather than hidden omissions.
- The current default tree still has optional dependency leaves represented as
  checked `configure_disabled` manifest rows when the configure option is off.
  Dependency-enabled threaded evidence now exists for the main optional
  external-library matrix in `/tmp/phase16-optional-deps-src`, configured
  against locally extracted packages in `/tmp/phase16-debroot` with
  `--enable-tap-tests`, `--with-ssl=openssl`, `--with-gssapi`, `--with-ldap`,
  `--with-selinux`, `--with-uuid=e2fs`, `--with-libcurl`, `--with-libxml`, and
  `--with-libxslt`. The threaded `TEMP_CONFIG` for these runs is
  `/tmp/phase16-optional-deps-src/src/test/regress/threaded_smoke.conf`, which
  sets `multithreaded=on`, `io_method=sync`, and `summarize_wal=off`.
- Optional-deps threaded checks passing in that scratch build:
  `MALLOC_CHECK_=3 gmake -C contrib/pgcrypto check`
  (`/tmp/phase16-optional-pgcrypto.log`);
  `MALLOC_CHECK_=3 gmake -C contrib/uuid-ossp check`
  (`/tmp/phase16-optional-uuid-ossp.log`);
  `MALLOC_CHECK_=3 gmake -C contrib/xml2 check`
  (`/tmp/phase16-optional-xml2.log`);
  `MALLOC_CHECK_=3 PG_TEST_EXTRA=ssl gmake -C src/test/ssl check`
  (`/tmp/phase16-optional-ssl.log`), which includes
  `src/test/ssl/t/003_sslinfo.pl` and `EXTRA_INSTALL=contrib/sslinfo`;
  `MALLOC_CHECK_=3 gmake -C src/test/modules/ssl_passphrase_callback check`
  (`/tmp/phase16-optional-ssl-passphrase.log`);
  `LANG=C MALLOC_CHECK_=3 gmake -C src/test/icu check`
  (`/tmp/phase16-optional-icu.log`);
  `MALLOC_CHECK_=3 gmake -C src/interfaces/libpq-oauth check`
  in a cassert-enabled libcurl scratch tree
  (`/tmp/phase16-libpq-oauth-cassert-src`), with evidence log
  `/tmp/phase16-libpq-oauth-cassert.log`;
  `MALLOC_CHECK_=3 PG_TEST_EXTRA=ldap gmake -C src/test/ldap check`
  (`/tmp/phase16-optional-ldap.log`);
  `MALLOC_CHECK_=3 PG_TEST_EXTRA=ldap gmake -C src/test/modules/ldap_password_func check`
  (`/tmp/phase16-optional-ldap-password-func.log`); and
  `MALLOC_CHECK_=3 PG_TEST_EXTRA=kerberos gmake -C src/test/kerberos check`
  (`/tmp/phase16-optional-kerberos.log`).
- The optional-deps work admitted thread-per-session module metadata for
  `contrib/pgcrypto`, `contrib/uuid-ossp`, `contrib/xml2`,
  `contrib/sslinfo`, `contrib/sepgsql`,
  `src/test/modules/ldap_password_func`, and
  `src/test/modules/ssl_passphrase_callback`. It also moved
  `ssl_passphrase_callback`'s mutable passphrase pointer into
  extension-private session state, added test-scoped OpenLDAP helper overrides
  (`PG_TEST_OPENLDAP_SLAPD`, `PG_TEST_OPENLDAP_SCHEMA_DIR`), added
  test-scoped Kerberos helper overrides (`PG_TEST_KRB5_BIN_DIR`,
  `PG_TEST_KRB5_SBIN_DIR`, `PG_TEST_KRB5_KDB_MODULE_DIR`), and documented that
  ICU threaded checks need `LANG=C` so process `LC_CTYPE` matches the temporary
  database locale.
- `contrib/sepgsql` is configured and built in the optional-deps scratch tree,
  but full policy TAP remains blocked on this host because SELinux is disabled
  and the file-context database is unavailable: `sestatus` reports disabled,
  `matchpathcon -n .` cannot open the file contexts database, and
  `getsebool sepgsql_regression_test_mode` reports SELinux is disabled. After
  adding thread-per-session module metadata and splitting threaded
  shared-preload replay from runtime-global hook installation, a clean manual
  threaded postmaster boot with `shared_preload_libraries='sepgsql'` passes and
  `psql` reports `shared_preload_libraries=sepgsql` and `multithreaded=on`; see
  `/tmp/phase16-sepgsql-load-clean-postmaster.log` and
  `/tmp/phase16-sepgsql-load-clean-psql.log`.
- PL/Python and PL/Tcl have separate dependency-enabled threaded evidence from
  scratch trees: `/tmp/phase16-python-worktree`, configured with
  `--with-python`, local headers from `/tmp/phase16-local-deps/root`, and
  threaded `TEMP_CONFIG`, passes `MALLOC_CHECK_=3 gmake -C src/pl/plpython
  check` with all 23 PL/Python regression tests plus sequential
  `MALLOC_CHECK_=3` threaded checks for `contrib/hstore_plpython`,
  `contrib/jsonb_plpython`, and `contrib/ltree_plpython`;
  `/tmp/phase16-tcl-worktree`, configured with `--with-tcl`, local Tcl 8.6.14
  under `/tmp/phase16-tcl-prefix`, and threaded `TEMP_CONFIG`, passes
  `MALLOC_CHECK_=3 gmake -C src/pl/tcl check` with all 8 PL/Tcl regression
  tests. The optional language rows remain `configure_disabled` in this
  default build but are no longer release-blocking.
- Injection-point optional coverage now has dependency-enabled threaded
  evidence from `/tmp/phase16-injection-src`, configured with `--without-icu
  --enable-tap-tests --enable-injection-points`. After hardening
  `src/test/modules/injection_points` for thread-per-session module loading,
  SQL-visible backend identity, and per-session DSM attachment state,
  `MALLOC_CHECK_=3 gmake -C src/test/modules/injection_points check
  TEMP_CONFIG=/tmp/phase16-injection-src/src/test/regress/threaded_smoke.conf`
  passes its regression and isolation suites; the dependent injection-point
  leaves also pass with `MALLOC_CHECK_=3 gmake -C src/test/modules/gin check`
  and `MALLOC_CHECK_=3 gmake -C src/test/modules/typcache check` under the same
  threaded `TEMP_CONFIG`. These rows remain `configure_disabled` only because
  the default build has `enable_injection_points=no`.
- TAP-enabled process-mode `check-world` also passes in `/tmp/phase16-tap-src`,
  configured with `--without-icu --with-perl --enable-tap-tests
  PG_TEST_EXTRA=`. The evidence log is
  `/tmp/phase16-tap-check-world-after-pg-upgrade-config.log`; it includes
  recovery `t/009_twophase.pl` and `t/027_stream_regress.pl`,
  `contrib/basebackup_to_shell/t/001_basic.pl`, `src/bin/pg_basebackup`, and
  `src/bin/pg_upgrade/t/002_pg_upgrade.pl`.
- ASAN evidence exists from `/tmp/phase16-asan-src`, configured with
  `--enable-cassert --enable-debug`, `CFLAGS=-O1 -g -fsanitize=address
  -fno-omit-frame-pointer`, and `LDFLAGS=-fsanitize=address`. With
  `ASAN_OPTIONS=detect_leaks=0`, both `check-threaded-smoke` and targeted
  recovery `t/027_stream_regress.pl` passed after the Phase 16 teardown fixes.
- TSAN was attempted in `/tmp/phase16-tsan-src` with GCC 13 and
  `-fsanitize=thread`, but configure could not run even its trivial test
  executable because the TSAN runtime terminated with `FATAL:
  ThreadSanitizer: unexpected memory mapping`. `clang` was not installed in
  this workspace, so TSAN is currently recorded as a platform/toolchain
  limitation rather than a PostgreSQL test result.
- Focused Phase 16 performance evidence is recorded in
  `MULTITHREADED_BENCHMARKS.md`, including process, thread-per-session, and
  pooled protocol lanes. Hot tiny-query overhead remains a known optimization
  bucket; mostly-idle and stateful pooled protocol profiles remain close to
  process/threaded throughput while using fewer server threads.
- Lock-order and debug-surface evidence is recorded in
  `MULTITHREADED_PHASE16_LOCKS_AND_DEBUG.md`. The only currently allowed
  Phase 16 cross-lock order is `DynamicFileManagerMutex -> ThreadedGUCMutex`,
  used by dynamic-library initialization and threaded config replay when
  modules define custom GUCs.

## Gate G Closeout Audit

Gate G is closed for Phase 16 on this branch as of the June 27, 2026 closeout
audit. The local default-configure closure command is
`MALLOC_CHECK_=3 gmake check-phase16-gate-g-local`, which rebuilds
`tmp_install`, runs process-mode `check-world`, runs `check-world-threaded`,
and then runs the lifecycle and global-lifetime gates. The current evidence log
is `/tmp/phase16-gate-g-local-fresh-temp-install.log`, with status `0`.

Exit-criteria mapping:

- coverage contract: `check-threaded-world-coverage` passes with `141 covered`,
  `22 excluded`, and `142 enabled leaves`;
- manifest status: all 22 exclusion rows have `release_blocker=no`; 21 are
  `configure_disabled` rows and one is the support-only `src/test/perl`
  `not_applicable` row;
- non-contrib coverage: the local Gate G bundle covers enabled `src/test`,
  `src/pl`, `src/interfaces`, `src/bin`, and `src/tools/pg_bsd_indent` leaves,
  while optional/configure-gated leaves have dependency-enabled evidence or an
  explicit platform row;
- contrib and bundled PL coverage: admitted contrib and PL leaves pass in the
  local threaded world, with separate dependency-enabled evidence for
  PL/Python, PL/Tcl, pgcrypto, uuid-ossp, xml2, sslinfo, ICU, LDAP, Kerberos,
  injection-point modules, ssl passphrase, ldap password, and libpq-oauth;
- custom GUC and extension failure paths: the backend-runtime Phase 16 stress
  test covers load failure, `_PG_init()` failure, custom GUC rollback,
  extension `ERROR`, `FATAL`, cancel, reconnect, and continued server
  usability;
- process-mode compatibility: process-mode `check-world` passes as part of the
  local Gate G bundle, and TAP-enabled process `check-world` has separate
  scratch evidence;
- lifecycle/global discipline: `check-runtime-lifecycles` and
  `check-global-lifetimes` pass as part of the local Gate G bundle;
- debug and lock-order evidence: `MULTITHREADED_PHASE16_LOCKS_AND_DEBUG.md`
  records the required lock ordering and explains why the current manifest,
  coverage, lifecycle, global-lifetime, and component-named test output are the
  sufficient debug surface for static module admission;
- sanitizer evidence: ASAN smoke and targeted recovery evidence exists; TSAN
  was attempted with GCC 13 but the runtime cannot execute even configure's
  trivial test program on this host, and non-interactive system installation of
  an alternate clang toolchain is blocked by `sudo` requiring a password;
- performance evidence: `MULTITHREADED_BENCHMARKS.md` records process,
  thread-per-session, and pooled protocol Phase 16 baselines, with hot
  tiny-query overhead classified as a continuing optimization bucket rather
  than a Gate G blocker.

Accepted external rows:

- `contrib/sepgsql`: module metadata and threaded preload smoke pass in a
  with-selinux scratch build, but full policy TAP requires an SELinux-enabled
  host with file-context policy and `sepgsql_regression_test_mode`;
- `src/bin/pgevent`: Windows-only event-log helper coverage requires a Windows
  build where `PORTNAME=win32`;
- configure-disabled optional leaves remain checked manifest rows in this
  default build, with replacement guards pointing at dependency-enabled
  evidence where the host can provide it.

## Workstream Ordering

Recommended implementation order:

1. Baseline and branch hygiene.
2. Coverage contract and exclusion manifest.
3. `check-world-threaded` discovery target.
4. World inventory generation.
5. Extension/custom GUC semantics.
6. Non-contrib `src/test`, `src/pl`, `src/interfaces`, `src/bin`, and tool
   mapping.
7. Low-risk contrib admission.
8. Hook/GUC contrib admission.
9. Shared-memory, worker, stats, and logical module admission.
10. PL and external runtime admission.
11. Failure-path stress.
12. Debug views and lock documentation.
13. Sanitizers.
14. Performance baseline and Gate G closeout.

This order is intentionally front-loaded with harness and coverage work. It
prevents the phase from turning into unbounded module-by-module cleanup without
a measurable definition of done.

## Gate G Exit Criteria

Phase 16 is complete only when:

- `check-world-threaded` mechanically covers every `check-world` component
  except manifest exclusions;
- every manifest exclusion is explicit, current, and justified;
- the remaining manifest is empty or contains only consciously accepted
  release blockers;
- non-contrib `check-world` components under `src/test`, `src/pl`,
  `src/interfaces`, `src/bin`, and `src/tools/pg_bsd_indent` are covered by a
  threaded target or explicit manifest row;
- threaded contrib regression passes for every admitted contrib module;
- bundled procedural-language checks pass for every admitted language;
- custom and extension GUC stress passes;
- extension and module load, waits, interrupts, `ERROR`, `FATAL`, cancel,
  terminate, reconnect, and teardown stress passes;
- process-mode `check-world` remains green;
- lifecycle and global-lifetime checks remain green;
- required debug views and lock-order notes are present for any new Phase 16
  runtime locks or admission state;
- sanitizer evidence has been recorded where feasible;
- performance baselines have been recorded and any regression is classified.

The local source tree now exposes `check-phase16-gate-g-local` as the repeatable
local closeout bundle for the default configure. It runs process-mode
`check-world`, `check-world-threaded`, `check-runtime-lifecycles`, and
`check-global-lifetimes`, and depends on a fresh top-level `temp-install` so
process and threaded submakes do not accidentally reuse stale test modules from
an older temporary install. This target is deliberately local: optional
dependency builds, sanitizer runs, Windows-only `pgevent`, SELinux policy
coverage, and performance baselines remain recorded as separate Gate G evidence
because they require different configure options or host capabilities.

Gate G should not pass with unknown/default process-only bundled modules. A
module may remain process-only only when the manifest says that explicitly and
the project accepts the release impact.
