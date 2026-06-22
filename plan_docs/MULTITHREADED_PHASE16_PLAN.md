# Phase 16 Plan: Threaded World And Extension Hardening

Phase 16 closes Gate E2-Extensions / Gate G after the Phase 15 pooled
protocol scheduler is complete. The goal is to make threaded mode credible for
the whole bundled PostgreSQL tree: contrib extensions, bundled procedural
languages, test modules, extension GUCs, hooks, and failure paths.

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

- contrib-wide threaded regression coverage;
- bundled procedural languages beyond PL/pgSQL;
- custom and extension GUC ownership, hook, and replay semantics;
- extension backend-model metadata and compatibility levels;
- extension-owned runtime/session APIs needed by in-tree modules;
- threaded stress coverage for extension load, unload, cancellation, teardown,
  `ERROR`, `FATAL`, and postmaster shutdown behavior;
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
- `check-threaded-test-modules`: threaded test-module target where applicable.
- `check-threaded-interfaces`: threaded interface/tool coverage where relevant
  to `check-world`.
- `check-threaded-world-coverage`: verifier for the strict coverage contract.
- `plan_docs/MULTITHREADED_PHASE16_EXCLUSIONS.tsv`: checked manifest of
  temporary or permanent exclusions.
- A generated or maintained inventory for contrib, PL, and test modules.

The first version of `check-world-threaded` may expose many blockers. That is
acceptable only if the coverage verifier is strict and every omission is named.

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
- `test-module`
- `bin`
- `interface`
- `isolation`
- `tap`
- `other`

Suggested `status` values:

- `temporarily_blocked`
- `process_only_by_design`
- `release_blocker`
- `not_applicable`

Rules:

- Every skipped `check-world` component must have one manifest row.
- Every row must name a concrete technical reason.
- Every temporary blocker must say whether it blocks release.
- A `process_only_by_design` row must explain why threaded mode should never
  admit that component.
- A `not_applicable` row must explain why the `check-world` component has no
  meaningful threaded equivalent.
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

## Step 1: Define Threaded World Coverage

Add the formal coverage rule to the plan and build system:

```text
check-world-threaded = check-world - explicit_manifest_exclusions
```

Implementation direction:

- identify the components that `check-world` runs in this tree;
- define the threaded equivalent target list;
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
excluded: src/pl/plpython release_blocker "Python interpreter thread-state audit"
missing: contrib/example_module
stale: contrib/removed_module
```

## Step 3: Add Initial `check-world-threaded`

Start with a target shape like:

```make
check-world-threaded:
	$(MAKE) check-threaded-world-core
	$(MAKE) check-threaded-contrib
	$(MAKE) check-threaded-pl
	$(MAKE) check-threaded-test-modules
	$(MAKE) check-threaded-interfaces
	$(MAKE) check-threaded-world-coverage
```

Keep target names specific. If a subtarget does not yet cover all components in
its category, its manifest and verifier must make that explicit.

## Step 4: Generate The Extension Inventory

Create an inventory for each bundled module. It can start as TSV or generated
markdown, but it must be precise enough to drive implementation order.

Track at least:

- component path;
- build target and test target;
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

This inventory should decide the order of work. Avoid picking modules manually
once the inventory can identify lower-risk tranches.

## Step 5: Finish Extension GUC Semantics Early

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

## Step 6: Admit Low-Risk Contrib Modules

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

## Step 7: Admit Hook And GUC Extensions

Next handle modules whose risk is mostly hooks, GUCs, or per-session policy.

Likely candidates:

- `auth_delay`
- `auto_explain`
- `basebackup_to_shell`
- `basic_archive`
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

## Step 8: Admit Shared-Memory, Stats, Worker, And Logical Modules

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

## Step 9: Admit External Runtime And Security Modules

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

For security and external-library modules:

- document external thread-safety assumptions;
- synchronize or isolate process-global mutable state;
- retain a release-blocking manifest row until tests prove the assumption.

## Step 10: Add Failure-Path Stress

Add extension-focused failure coverage after enough modules are admitted for
the tests to matter.

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

## Step 11: Add Sanitizer Runs

Run sanitizers where feasible and document any platform-specific limitations.

Expected work:

- ASAN build and selected full suites;
- TSAN build if practical, with reviewed suppressions for known PostgreSQL or
  platform noise;
- repeated threaded-world runs under normal builds;
- targeted stress for cancellation, teardown, waits, and extension hooks.

Sanitizer results should be part of Gate G evidence, even if the exact target
set is smaller than ordinary `check-world-threaded`.

## Step 12: Record Performance Baselines

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

## Workstream Ordering

Recommended implementation order:

1. Baseline and branch hygiene.
2. Coverage contract and exclusion manifest.
3. `check-world-threaded` discovery target.
4. Inventory generation.
5. Extension/custom GUC semantics.
6. Low-risk contrib admission.
7. Hook/GUC contrib admission.
8. Shared-memory, worker, stats, and logical module admission.
9. PL and external runtime admission.
10. Failure-path stress.
11. Sanitizers.
12. Performance baseline and Gate G closeout.

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
- threaded contrib regression passes for every admitted contrib module;
- bundled procedural-language checks pass for every admitted language;
- custom and extension GUC stress passes;
- extension load, `ERROR`, `FATAL`, cancel, terminate, reconnect, and teardown
  stress passes;
- process-mode `check-world` remains green;
- lifecycle and global-lifetime checks remain green;
- sanitizer evidence has been recorded where feasible;
- performance baselines have been recorded and any regression is classified.

Gate G should not pass with unknown/default process-only bundled modules. A
module may remain process-only only when the manifest says that explicitly and
the project accepts the release impact.
