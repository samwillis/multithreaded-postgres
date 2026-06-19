# Multithreaded PostgreSQL Current Status

This is a handoff summary of the recent Phase 13/Phase 14 work. It is meant
to help another pass decide what is solid, what is experimental, and what must
be fixed before moving on.

## Repository State

As of this note, the branch is `multithreaded` and is ahead of origin by three
commits. The working tree is dirty. Treat the current tree as an experimental
state, not as a clean checkpoint.

The dirty work includes:

- README/project documentation updates.
- Phase 13 wait-completion changes.
- Phase 14 pooled-scheduler prototype changes.
- A new focused Phase 14 TAP test.
- Generated regression/test output directories from local runs.

Do not assume every dirty change should be committed as-is. The code should be
split into reviewable pieces before a final commit.

## What Has Been Done

### Threaded Wait-Completion Substrate

Phase 13 introduced scheduler-visible wait-completion records around regular
backend waits.

Implemented or exercised families include:

- `WaitEventSetWait()`, covering latches, sockets, frontend input/output, and
  timeout-backed waits.
- `WaitLatch()` and `WaitLatchOrSocket()` through the event-set path.
- `ProcWaitOnSemaphore()`, covering semaphore-backed waits such as LWLocks and
  related PGPROC semaphore users.
- Heavyweight/advisory lock waits through existing latch wait paths.
- Condition variable waits through existing latch wait paths.

The important distinction is that these waits are now observable and can be
marked ready through the wait-completion layer. That does not mean every wait
is a true scheduler-yielding async boundary.

### Scheduler Queue Wake Fix

The pooled scheduler socket wake path had a queue mutation race. The prototype
now claims a matching socket wait while holding the scheduler lock, detaches it
from the waiting queue, marks it running, and then wakes it.

This removed a spin/hang observed in the focused Phase 14 TAP test.

### Pooled Scheduler Latch Ownership Fix

The focused Phase 14 prototype needed pooled backends to use a carrier/scheduler
latch when they are attached to a carrier. Without that, deep waits such as
`pg_sleep()` could publish wait-completion records and accept cancel requests,
but the physical wait was still using a stale backend latch and would not wake
until natural timeout.

The current prototype installs the carrier scheduler latch into the attached
backend and updates the frontend/backend wait set latch entry. It also adjusts
early fallback proc-pid handling so local carrier latches have a sensible owner
identity during attach/detach transitions.

### Focused Validation That Passed

The following validation was observed passing after the recent fixes:

```sh
git diff --check
gmake -j$(nproc)
gmake -C src/test/modules/test_backend_runtime check
gmake check-threaded-world-core-tap \
  THREADED_WORLD_CORE_TAP_TESTS='t/004_phase13_wait_completion.pl t/005_phase14_pooled_scheduler.pl'
gmake check-runtime-lifecycles
gmake check-global-lifetimes
gmake check
```

The focused TAP coverage included frontend input waits, frontend output waits,
`pg_sleep()`, statement timeout, advisory lock cancel/wake behaviour, and
multiple pooled clients.

## What Is Not Working

### Full Threaded Regression Hangs

`gmake check-threaded` is not currently green. It hung in the core regression
suite around the `tablespace` test.

The observed SQL wait was:

```sql
ALTER DATABASE regression_utf8 SET TABLESPACE regress_tblspace;
```

`pg_stat_activity` showed the client backend waiting on:

```text
IPC / CheckpointStart
```

The checkpointer thread was asleep in the usual latch wait path. Manually
calling `WakeupCheckpointer()` in gdb did not free the waiter.

The best current hypothesis is that this is a threaded auxiliary-process
PGPROC/latch wakeup problem, not a simple wait-completion publication problem.
Possibilities include:

- `ProcGlobal->checkpointerProc` points to the wrong or stale `PGPROC`.
- The requester wakes one latch while the checkpointer waits on another.
- Shared latch owner metadata is stale after threaded auxiliary-process setup.
- `SetLatch()` sees the same OS pid and relies on sibling-thread wakeup
  metadata that is not correct for this auxiliary process.

This must be debugged before claiming the threaded baseline is healthy.

### Remaining Validation Is Not Complete

Because `check-threaded` hangs, these broader gates should be considered not
yet validated:

- `gmake check-threaded`
- `gmake check-threaded-workers`
- `gmake check-threaded-world-core`
- any performance work based on the current dirty state

## Design Caveat

The recent Phase 13 work makes waits visible and externally completable. It
does not by itself make arbitrary PostgreSQL C call stacks suspendable.

For now, deep waits can still block the current carrier even though they
publish wait-completion records. A future pooled scheduler must only detach a
logical backend at places where it can return normally to the scheduler, unless
the project deliberately chooses a stackful coroutine/fiber design.

Current safest scheduler-yielding boundary:

- top-level frontend input after a command completes

Potential but not yet designed:

- frontend output backpressure, if the output path can return a would-block
  result to a safe boundary

Not solved by Phase 13:

- arbitrary lock waits
- checkpoint waits
- condition variable waits
- LWLock waits
- `pg_sleep()`

Those may be observable and cancellable, but not necessarily carrier-yielding.

## Missteps And Process Notes

The recent work was allowed to run too long without a clean checkpoint. The
dirty tree is now large enough that it should be split before more coding.

Specific process issues:

- Focused TAP success was not enough to infer full threaded readiness.
- A broad dirty tree made it harder to isolate the `check-threaded` regression.
- Some test targets were run in parallel earlier, causing temporary
  `tmp_install`/test-output conflicts. The affected focused TAP was rerun
  cleanly and passed.
- The Phase 14 pooled scheduler prototype mixed foundational latch ownership
  fixes with broader scheduler-carrier work. Those should be separated.

## Recommended Next Steps

1. Preserve this dirty tree or branch it before any cleanup.
2. Remove generated test output from the candidate commit set.
3. Split the code into logical review chunks:
   - docs
   - Phase 13 wait-completion substrate
   - focused tests
   - pooled scheduler prototype
   - latch ownership fixes
4. Reduce the `check-threaded` hang to a smaller checkpointer/latch reproducer.
5. Instrument:
   - `InitAuxiliaryProcess()`
   - `ProcGlobal->checkpointerProc`
   - `WakeupCheckpointer()`
   - checkpointer `MyProc`, `MyProcNumber`, `MyLatch`, and `procLatch`
   - latch owner pid, wake fd, and owner thread metadata
6. Fix the threaded checkpointer wake path.
7. Rerun the full validation gate before starting broader async-boundary work.

## Bottom Line

Phase 13 has useful wait-completion machinery and focused tests pass. The
current branch does not yet have a fully validated foundation for Phase 14
async-boundary scheduling. The immediate blocker is the full threaded
regression hang in the checkpoint path.
