# Phase 16B Plan: Warm Session Pool And Lifecycle Offload

Phase 16B is a performance and architecture phase after Phase 16 threaded-world
closure and before any Phase 17 deep scheduler-boundary work. Its purpose is to
decide, with benchmark evidence, whether connection churn and pooled protocol
latency can be improved by moving lifecycle work off the foreground connection
path and by keeping clean warm session capacity ready.

The long-term product direction is:

```text
PostgreSQL-native warm session pooling:
fast like PgBouncer, but with server-owned isolation, reset validation, and
fail-closed cleanup.
```

This phase must not trade correctness for speed. Configuration may choose more
or less memory, more or less prewarming, and more or less reuse, but it must not
allow a dirty session to be handed to a new client.

## Motivation

Phase 16 benchmarks show that mostly-idle and stateful pooled protocol profiles
remain close to the desired shape, but connection churn regressed materially
from the Phase 15 baseline. The likely cost is cumulative:

- logical connection startup still runs substantial backend initialization;
- threaded logical exit must explicitly clean state that process exit normally
  leaves to the operating system;
- pooled protocol attach, detach, park, resume, and current-work rebinding add
  small repeated costs;
- private memory context destruction and allocator contention may be visible in
  short-lived sessions.

Phase 16B investigates whether these costs can be reduced by:

- prebuilding clean session/runtime shells;
- asynchronously reclaiming detached private memory;
- eventually keeping fully initialized clean idle backends keyed by pool
  identity.

## Non-Goals

Phase 16B does not own:

- SQL-level transaction pooling semantics;
- skipping authentication;
- reusing a session that has not passed hard reset validation;
- arbitrary third-party extension reuse without metadata, hooks, or a destroy
  fallback;
- deep executor, COPY, lock-wait, storage, or frontend-output yielding;
- weakening process-mode behavior or Phase 16 `check-world-threaded`
  guarantees.

## Pooling Vocabulary

Use separate names for separate risk levels.

- `fresh`: current behavior. A logical backend is initialized for one client
  and destroyed after disconnect.
- `warm_shell`: prebuilt runtime/session/backend/connection/execution object
  storage and safe memory scaffolding, with no authenticated user, database,
  procarray membership, socket, or externally visible backend identity.
- `warm_backend`: a clean idle initialized backend keyed by pool identity and
  ready to accept a compatible authenticated client after validation and client
  attachment.
- `retired`: a disconnected session that is no longer externally visible and
  is waiting for private cleanup, validation, destruction, or reuse.
- `quarantined`: a retired session that failed validation or used unsupported
  extension/runtime state. It must be destroyed, not reused.

## Configurable Policy

The feature should be policy-driven. Proposed GUC shape:

```conf
threaded_session_pool = off | shell | warm_backend
threaded_session_pool_min = 0
threaded_session_pool_max = 64
threaded_session_pool_memory_limit = '512MB'
threaded_session_pool_idle_timeout = '5min'
threaded_session_pool_prewarm = on
threaded_session_pool_janitor_workers = 1
threaded_session_pool_validation = strict
threaded_session_pool_extension_policy = destroy | hook_required | reject
```

Possible future GUCs:

```conf
threaded_session_pool_key = database,user,security,client_encoding
threaded_session_pool_target_connect_latency = '2ms'
threaded_session_pool_warm_backend_min_per_key = 0
threaded_session_pool_warm_backend_max_per_key = 16
```

Default policy should be conservative:

```conf
threaded_session_pool = shell
threaded_session_pool_min = 0
threaded_session_pool_max = min(max_connections / 4, 32)
threaded_session_pool_validation = strict
threaded_session_pool_extension_policy = destroy
```

Users who prefer low memory can set `threaded_session_pool = off`. Users who
prefer PgBouncer-style latency can opt into `warm_backend` and allocate a
larger pool and memory budget.

## Correctness Contract

The reset contract is fail-closed.

A session may return to any reusable pool only after all required validators
pass:

- no transaction is active or aborted;
- no locks, LWLocks, advisory locks, or predicate locks are held;
- no buffer pins or local buffer pins remain;
- no snapshots, portals, cursors, prepared statements, or resource owners
  remain live;
- no temp namespace, temp files, temp tables, large-object state, LISTEN state,
  or async-notify state remains;
- no active timeout, interrupt, wait, signal, latch, procarray, procsignal, or
  shared-invalidation ownership remains from the prior client;
- no open FDs or socket ownership remains from the prior client;
- GUC state is back to the pool baseline, including startup-packet and
  `PGOPTIONS` changes;
- user, database, security, SSL/GSS/client metadata, `application_name`, and
  client encoding are reset or match the pool key;
- extension private state is either proven reset-safe or forces destroy;
- memory roots match the expected empty or reusable shape.

If validation fails, destroy the session. Do not add a configuration option to
reuse failed sessions.

## Stage 0: Baseline And Branch Hygiene

Goal: start from a known Phase 16 closeout point.

Tasks:

- Record the exact Phase 16 HEAD and local tree state.
- Re-run the existing correctness gate:

```text
PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3 gmake check-phase16-gate-g-local
git diff --check
```

- Record current benchmark evidence from:
  - `connection_churn`;
  - `connection_churn_realish`;
  - `pinned_hot`;
  - `pool_idle_100ms`;
  - `pool_stateful_1000ms`;
  - `connection_memory_idle`.
- Confirm the benchmark install is non-cassert and not using stale
  `tmp_install` artifacts.

Exit criteria:

- correctness gate passes;
- baseline result directory is recorded in `MULTITHREADED_BENCHMARKS.md`;
- benchmark variance is measured with at least three runs for churn profiles.

## Stage 1: Lifecycle Cost Instrumentation

Goal: determine whether warm pooling can plausibly help before building it.

Add low-overhead optional counters/timers behind a disabled-by-default GUC or
environment flag.

Measure startup:

- logical shell acquisition/allocation;
- `MemoryContextInit`;
- runtime object initialization/adoption;
- `InitializeTransactionState`;
- threaded GUC setup and config replay;
- `pq_init` and socket/Port setup;
- startup packet read;
- authentication;
- `InitProcessPhase2`;
- module replay and deferred shmem callbacks;
- pgstat startup;
- shared invalidation startup;
- relcache/catcache/plancache initialization;
- `InitPostgres` total;
- first `ReadyForQuery`.

Measure teardown:

- `shmem_exit`;
- `on_proc_exit` callback loop;
- socket close and Port/socket I/O cleanup;
- connection reset buckets;
- session reset buckets, with per-bucket timing;
- backend reset buckets, with per-bucket timing;
- execution reset buckets, with per-bucket timing;
- retained `TopMemoryContext` deletion;
- AllocSet freelist drain;
- `malloc_trim`;
- postmaster logical-exit publication.

Measure scheduler and rebinding:

- attach/detach count and time;
- `PgRuntimeSetCurrentWork` time;
- hot-field and fast-bucket fallback counters;
- park/mark-runnable/lease/repark spinlock time;
- parked-read poll scan count, duplicate-socket checks, and poll wait time;
- same-carrier versus migrated resume counts.

Exit criteria:

- instrumentation overhead is measured with instrumentation disabled and
  enabled;
- churn profile reports a per-connection startup and teardown breakdown;
- pooled profile reports per-park/resume scheduler and rebinding breakdown.

Decision gate:

- If prebuildable shell work plus private teardown is below roughly 5 percent
  of connection-churn wall time, do not build the shell pool yet.
- If semantic startup dominates, prioritize `warm_backend` design and validator
  work over shell-only pooling.
- If private memory destruction or allocator contention is visible, build the
  janitor before or alongside shell pooling.

## Stage 2: Proof-Of-Value Benchmarks

Goal: estimate upper bounds before committing to the full feature.

Run controlled benchmark experiments:

- disable or defer safe private cleanup in a debug-only experiment, with memory
  limits and no correctness claims, to estimate janitor upside;
- preallocate logical-state shells in a synthetic benchmark path to estimate
  shell-pool upside;
- enable bridge fallback stats and verify hot fallback counters stay near zero
  in churn and pooled profiles;
- profile with call graphs to identify exact `hash_search_with_hash_value`,
  `AllocSet*`, and startup/teardown call sites.

Required profiles:

```text
connection_churn
connection_churn_realish
pinned_hot
pool_idle_100ms
pool_burst_10ms
pool_stateful_1000ms
connection_memory_idle
```

Compare:

- vanilla;
- branch process;
- threaded fresh;
- pooled fresh;
- experimental deferred cleanup;
- experimental prebuilt shell.

Decision gate:

- Continue to implementation if the measured or estimated win is at least one
  of:
  - 10 percent or greater improvement in threaded `connection_churn`;
  - 10 percent or greater improvement in `connection_churn_realish`;
  - 20 percent or greater reduction in p95 connect-to-ready latency;
  - clear allocator-contention reduction without correctness regressions.
- Otherwise record the evidence and leave Phase 16B as an analysis phase.

## Per-Stage Benchmark Checkpoint

After each implementation stage, rerun the same focused benchmark slice before
starting the next stage. The slice is:

```text
connection_churn
connection_churn_realish
pinned_hot
pool_idle_100ms
pool_burst_10ms
pool_stateful_1000ms
connection_memory_idle
```

For each stage, record:

- result directory;
- enabled pool policy and GUCs;
- TPS and p95/p99 connect-to-ready latency;
- foreground disconnect latency;
- memory PSS/private RSS;
- server thread count;
- pool hit/miss/destroy/quarantine counters where applicable;
- janitor queue depth and queued bytes where applicable;
- validation pass/fail counts where applicable;
- correctness gate result.

Progression rule:

- continue when the stage improves its target metric or is neutral within
  measured variance and does not regress correctness or memory limits;
- stop and investigate when the stage regresses `pinned_hot` or steady-state
  pooled profiles outside variance;
- revert or disable-by-default when the stage does not improve its target
  metric and adds meaningful complexity;
- never continue past a validation leak or dirty-session reuse failure.

## Stage 3: Async Private Cleanup Janitor

Goal: move physical private-memory destruction off the foreground disconnect
path where safe.

Foreground disconnect must still perform semantic cleanup synchronously:

- transaction abort/commit cleanup;
- lock, pin, snapshot, procarray, procsignal, shared-invalidation, pgstat, and
  resource-owner cleanup;
- socket close and FD unregister;
- postmaster logical backend exit publication;
- current-pointer severing.

The janitor may handle only detached private objects:

- retained `TopMemoryContext` deletion;
- AllocSet freelist draining;
- `malloc_trim`;
- private memory context trees proven unreachable;
- retired shell destruction after semantic cleanup.

Required controls:

- bounded retire queue;
- byte-based memory budget;
- synchronous fallback when queue or memory limit is exceeded;
- janitor progress counters;
- shutdown drain path;
- crash-safe behavior if the janitor encounters an unexpected live owner.

Exit criteria:

- all existing Phase 16 gates pass;
- repeated connect/disconnect stress passes with queue pressure;
- janitor-disabled and janitor-enabled benchmarks are recorded;
- the per-stage benchmark checkpoint shows foreground disconnect latency,
  allocator contention, or churn throughput improved enough to justify keeping
  the janitor enabled as an option;
- memory does not grow beyond configured limits under churn.

## Stage 4: Warm Shell Pool

Goal: prebuild safe connection-independent logical backend shells.

Warm shells may include:

- `PgThreadBackendLogicalState` storage;
- initialized `PgBackend`, `PgSession`, `PgConnection`, and `PgExecution`
  structs;
- clean runtime bucket defaults;
- safe memory context scaffolding;
- message and row-description context scaffolding;
- default GUC structures that do not encode client/user/database state;
- generation counters and validation metadata.

Warm shells must not include:

- accepted socket or `Port`;
- authenticated user;
- selected database;
- procarray or procsignal membership;
- shared-invalidation backend slot;
- pgstat backend entry;
- cancel key advertised to a client;
- client startup-packet options;
- extension state that depends on a real backend.

Implementation direction:

- add a background warmer that keeps `min` clean shells available up to `max`;
- acquire a shell during threaded or pooled logical start;
- attach socket, `Port`, startup packet, latch, cancel key, and publication;
- run normal semantic startup after shell acquisition;
- on disconnect, either destroy the shell or return it to the pool after
  validator success;
- collect hit, miss, create, reuse, destroy, and quarantine counters.

Exit criteria:

- shell pool can be disabled and produces current behavior;
- strict validator prevents dirty shell reuse;
- shell pool improves or neutrally affects churn benchmarks within variance;
- the per-stage benchmark checkpoint records `off` versus `shell` with the same
  benchmark slice before warm-backend work starts;
- memory use is bounded by configured pool size and memory limit.

## Stage 5: Reuse Validator And Quarantine

Goal: make reuse a checked contract, not an assumption.

Add a validator framework with:

- per-subsystem validation hooks;
- a central reusable-session verdict;
- reason codes for destroy/quarantine;
- debug logging for first failure per reason;
- assert-build hard failures for impossible states;
- SQL/debug view or log summary for pool health.

Minimum validator areas:

- transaction/resource owners;
- locks and buffer pins;
- snapshots and portals;
- prepared statements and plan cache;
- temp namespace/files/tables;
- GUC baseline;
- extension private state;
- pgstat/procarray/procsignal/shared-invalidation state;
- socket/FD ownership;
- memory context roots.

Exit criteria:

- validator catches deliberately injected dirty-state tests;
- failed validation destroys the session and does not affect the next client;
- reason counts are visible in logs or a debug view;
- strict validation is enabled for all reuse modes.

## Stage 6: Warm Backend Pool Prototype

Goal: test the PgBouncer-competing model with PostgreSQL-owned isolation.

Pool key should start conservative:

```text
database + authenticated role + security class + baseline startup options
```

The first prototype should support a small key set and simple auth cases.

Flow:

1. Accept connection and authenticate the client.
2. Resolve target pool key.
3. Lease a validated idle backend for that key, or create a fresh backend.
4. Attach new client socket/Port and per-client transient state.
5. Serve the session.
6. On disconnect, run reset contract and validation.
7. Return the backend to the keyed idle pool or destroy it.

The pool must preserve isolation:

- no cross-client GUC leakage;
- no prepared statement leakage;
- no temp object leakage;
- no LISTEN/NOTIFY leakage;
- no advisory lock leakage;
- no role/database/security leakage;
- no extension private-state leakage unless the extension has a reset-safe
  contract.

Extension policy:

- `destroy`: default. If unsafe extension state appears, destroy the backend.
- `hook_required`: reusable only if all loaded modules provide reset hooks or
  metadata.
- `reject`: reject session reuse for keys that load unsupported modules.

Exit criteria:

- warm backend pool works for plain SQL workloads;
- pool key mismatch always creates or selects a separate backend;
- failed validator destroys instead of reusing;
- churn and connect latency improve enough to justify complexity;
- the per-stage benchmark checkpoint records `off`, `shell`, and
  `warm_backend` before any compatibility expansion beyond the first pool key
  set;
- `check-world-threaded`, lifecycle checks, and targeted pool tests pass.

## Stage 7: Benchmark Matrix And Acceptance

Benchmark with each policy:

```text
threaded_session_pool = off
threaded_session_pool = shell
threaded_session_pool = warm_backend
```

Use multiple pool sizes:

```text
min=0 max=0
min=0 max=32
min=16 max=64
min=64 max=256
```

Required metrics:

- TPS;
- average, p95, and p99 connect-to-ready latency;
- foreground disconnect latency;
- janitor queue depth and queued bytes;
- pool hit/miss rate;
- validation pass/fail/destroy counts;
- memory PSS/private RSS;
- server thread count;
- allocator contention profile;
- failed transactions;
- correctness gate status.

Acceptance targets:

- `warm_shell` should not regress correctness or steady-state hot-path
  throughput, and should improve churn if Stage 1 showed shell cost is
  material.
- `warm_backend` should materially improve connection churn and p95
  connect-to-ready latency for compatible pool keys.
- low-memory `off` mode must remain available and must not regress from the
  Phase 16 baseline.
- memory must stay within configured limits, with backpressure and destroy
  fallback under pressure.

## Test Plan

Correctness gates:

```text
gmake check
gmake check-world
gmake check-world-threaded
gmake check-runtime-lifecycles
gmake check-global-lifetimes
PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3 gmake check-phase16-gate-g-local
git diff --check
```

Targeted new tests:

- shell pool hit/miss and fallback behavior;
- janitor queue pressure and synchronous fallback;
- validator dirty-state injection;
- cross-client GUC isolation;
- cross-client role/database isolation;
- temp table, prepared statement, cursor, LISTEN/NOTIFY, advisory lock, and
  extension-state leakage tests;
- disconnect during startup;
- cancel/terminate during pooled reuse;
- `ERROR`, `FATAL`, and postmaster shutdown during cleanup;
- pool memory limit enforcement;
- shutdown drain with retired sessions queued.

Stress tests:

- repeated connect/disconnect with pool enabled;
- concurrent churn across multiple pool keys;
- churn with extension load/unload;
- churn with failed auth and startup-packet errors;
- long idle warm backend expiry;
- pool resizing under load.

## Documentation Deliverables

Phase 16B should update:

- `MULTITHREADED_BENCHMARKS.md` with baseline, proof-of-value, and final
  policy comparison runs;
- `MULTITHREADED_ARCHITECTURE.md` with lifecycle offload and pool ownership;
- `MULTITHREADED_PROTOCOL_SCHEDULER_DESIGN.md` if attach/detach or scheduler
  policy changes;
- `MULTITHREADED_PHASE16B_WARM_SESSION_POOL_PLAN.md` as decisions are made.

If warm backend pooling lands, add user-facing documentation for:

- configuration modes;
- memory/performance tradeoffs;
- extension policy;
- validation and destroy behavior;
- pool health observability.

## Recommended Commit Sequence

1. Add measurement-only lifecycle instrumentation and benchmark output.
2. Record baseline and proof-of-value benchmark evidence.
3. Add async janitor for detached private memory only, behind config.
4. Add reusable shell object type, pool, counters, and strict disable path.
5. Add shell validator and quarantine/destroy fallback.
6. Admit shell pooling for threaded and pooled logical starts.
7. Benchmark `off` versus `shell`; decide whether to continue.
8. Add reusable-session validator framework.
9. Add first warm backend keyed pool prototype for plain SQL workloads.
10. Add isolation tests and dirty-state injection tests.
11. Expand extension policy to destroy/hook-required/reject.
12. Benchmark `off`, `shell`, and `warm_backend`.
13. Update architecture and benchmark docs.
14. Run full Phase 16B correctness and performance gates.

Each commit must leave process mode buildable. Any reusable-session expansion
must have a destroy fallback in the same commit.
