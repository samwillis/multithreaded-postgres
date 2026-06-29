# Phase 16 Lock Ordering And Debug Surface

This note records the current Gate G evidence for Phase 16 runtime locks and
debug surfaces. It is intentionally narrow: it covers the process-wide locks
introduced or materially hardened while making bundled modules and
`check-world-threaded` credible.

The pthread mutexes below are runtime coordination locks, not SQL heavyweight
locks. They should protect short metadata mutation, publication, or snapshot
sections unless an explicit exception is documented here.

## General Rules

- Defer interrupts while holding these locks, and release them from
  `PG_FINALLY()` paths where an `ERROR` can occur.
- Do not add waits on client I/O, latches, condition variables, heavyweight
  locks, LWLocks, semaphores, or long-running user callbacks while holding
  these locks.
- Keep any new nested acquisition out of this set unless the order is recorded
  in this file in the same change.
- Do not rely on a carrier-local recursion counter as a general deadlock
  avoidance mechanism. The counters exist so existing recursive startup paths
  do not self-deadlock.
- If a new Phase 16 manifest exclusion depends on lock behavior, add the
  replacement guard or debug evidence here instead of leaving the rationale in
  test logs only.

The one currently allowed Phase 16 cross-lock order is:

```text
DynamicFileManagerMutex -> ThreadedGUCMutex
```

This order is required when dynamic library load or threaded-session replay
invokes `_PG_init()` and the module defines custom GUCs or reserves GUC
prefixes. Do not introduce the inverse order.

## Phase 16 Runtime Locks

| Lock | Source | Protects | Ordering rule |
| --- | --- | --- | --- |
| `ThreadedGUCMutex` | `src/backend/utils/misc/guc.c` | Runtime-global GUC descriptors, custom GUC prefix reservation, and shared GUC lookup/update metadata that cannot be made session-local. | May be acquired while `DynamicFileManagerMutex` is already held during module initialization or config replay. Do not acquire `DynamicFileManagerMutex` while holding it. |
| `ThreadedRelOptionsMutex` | `src/backend/access/common/reloptions.c` | Runtime-global custom reloption registry and the threaded reloptions memory context. | Independent. Do not nest with GUC, dynamic loader, hook, or worker locks without extending this document. |
| `DynamicFileManagerMutex` | `src/backend/utils/fmgr/dfmgr.c` | Dynamic library list, backend-model checks for loaded modules, and threaded-session replay of libraries loaded before the logical session existed. | Outermost lock for dynamic module load/replay. It may call `_PG_init()` while held, so any nested GUC work must follow `DynamicFileManagerMutex -> ThreadedGUCMutex`. |
| `PgPlanAdviceAdvisorHookListMutex` | `contrib/pg_plan_advice/pg_plan_advice.c` | Runtime-global `pg_plan_advice` advisor hook list. | Independent. Mutate or copy the list while held, then invoke advisor hooks after unlock. |

## Previously Established Locks

These locks remain part of the threaded runtime, but Phase 16 did not add a new
cross-lock order for them:

| Lock | Source | Notes |
| --- | --- | --- |
| `ThreadedBackendRegistryMutex` | `src/backend/utils/init/backend_runtime_backend.c` | Protects the threaded backend registry. Keep registry operations short and isolated. |
| `PMChildLogicalBackendMutex` | `src/backend/postmaster/pmchild.c` | Protects cross-thread publication between logical backends and the postmaster. The source file carries its ownership contract. |
| `pooled_protocol_queue_mutex` | `src/backend/postmaster/launch_backend.c` | Protects the pooled-protocol carrier queue and its condition variable. Condition waits here are queue-local scheduler waits, not extension/GUC lock nesting. |
| `backend_thread_malloc_trim_mutex` | `src/backend/postmaster/launch_backend.c` | Serializes backend-thread malloc trim work during teardown. |
| `PgLocaleMutex` | `src/backend/utils/adt/pg_locale.c` | Serializes process-global locale operations in threaded mode. Treat as independent from Phase 16 GUC/module locks. |

## Debug And Validation Surface

Phase 16 does not currently need a new broad SQL introspection view for module
admission state. The current debug surface is:

- `check-threaded-world-coverage`, which fails when an enabled `check-world`
  leaf is neither covered by `check-world-threaded` nor listed in the Phase 16
  exclusion manifest;
- `plan_docs/MULTITHREADED_PHASE16_COVERED_COMPONENTS.tsv`, which records the
  threaded target for covered leaves;
- `plan_docs/MULTITHREADED_PHASE16_EXCLUSIONS.tsv`, which records explicit
  exclusions, reasons, status, release-blocker state, and replacement guards;
- `check-runtime-lifecycles`, which validates runtime/session/backend/execution
  state ownership classifications;
- `check-global-lifetimes`, which rejects unclassified mutable globals and
  local runtime-boundary violations;
- existing backend/session observability such as `pg_stat_activity` for
  operational session/backend state;
- `check-world-threaded` output, which preserves component target names in
  failures so the failing leaf can be mapped back to the inventory.

This is enough for the current Phase 16 admission model because module
compatibility is static metadata plus manifest coverage, and the runtime state
needing validation is already checked by build targets. Add a SQL debug view
only if future Phase 16 work introduces dynamic admission, dynamic carrier
affinity, or extension state that operators must inspect while the server is
running.

## Closeout Rule

Gate G cannot close if a new Phase 16 runtime, extension, hook, GUC,
shared-memory, worker, interpreter, or admission-state lock exists without a
row in this document or a more specific linked document. Gate G also cannot
close if a new SQL debug view is the only practical way to diagnose a Phase 16
failure and the view has not been added.
