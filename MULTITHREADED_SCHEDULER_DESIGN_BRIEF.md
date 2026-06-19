# Multithreaded Scheduler Design Brief

This is not the final design document. It is a brief for the next agent or
design pass. Its job is to frame the problem clearly enough that the wider
scheduler design can be written without being trapped by the current dirty
implementation state.

## Target

The branch is trying to evolve PostgreSQL from one backend per process toward
an architecture where logical backend/session state can be run by different
physical carriers:

- existing process-backed PostgreSQL backends;
- one OS thread per session;
- eventually, a smaller pool of carrier threads running many logical sessions.

The long-term design should allow PostgreSQL to schedule logical backend work
without assuming that every SQL session permanently owns a process or thread.

## Existing References

Start by reading:

- `MULTITHREADED_ARCHITECTURE.md`
- `MULTITHREADED_PHASE13_PLAN.md`
- `MULTITHREADED_CURRENT_STATUS.md`
- `MULTITHREADED_PLAN.md`
- `MULTITHREADED_RUNTIME_LIFECYCLE.tsv`
- `README.md`

Also keep the project background in mind:

- PostgreSQL wiki pages on multithreading.
- Thomas Munro's PGConf.dev 2025 talk, "Investigating Multithreaded
  PostgreSQL".
- The existing branch's runtime objects: `PgRuntime`, `PgCarrier`,
  `PgBackend`, `PgSession`, `PgConnection`, and `PgExecution`.

Do not treat the current dirty code as the design. Treat it as evidence.

## Important Non-Goal

Do not assume arbitrary C stacks can be paused and later resumed.

Unless the project deliberately adopts stackful coroutines, fibers, or custom
stack switching, a carrier cannot return to the scheduler while preserving an
arbitrary deep PostgreSQL call stack. The safe default design is:

- the carrier owns the active C stack;
- a logical backend may detach only at a boundary where control can return
  normally to the scheduler;
- resumable state must live in explicit heap/runtime structures;
- the scheduler later reattaches the backend and re-enters through a known
  continuation point.

This is the central constraint for Phase 14 and later work.

## Terms To Nail Down

The full design should define these terms precisely:

- `PgRuntime`: address-space/runtime-wide state and registries.
- `PgCarrier`: physical execution vehicle, such as process or OS thread.
- `PgBackend`: logical backend identity for cancellation, stats, lifecycle,
  interrupts, and lock participation.
- `PgSession`: SQL session state that should survive carrier movement.
- `PgConnection`: frontend connection/socket/protocol state.
- `PgExecution`: per-command or active execution state.
- Observable wait: a wait that publishes metadata but still blocks the carrier.
- Scheduler-yielding wait: a wait that detaches the logical backend and returns
  the carrier to the scheduler.
- Carrier-blocking wait: a traditional blocking wait, possibly still visible
  through wait-completion metadata.

## Suggested Design Document Shape

The next design pass should flesh out a document with roughly this structure.

### 1. Goals

Describe the intended end state:

- process mode continues to work;
- thread-per-session mode remains a compatibility stepping stone;
- pooled scheduler mode can eventually run many logical sessions on fewer
  carrier threads;
- cancellation, timeout, stats, lock waits, and frontend IO remain correct.

### 2. Non-Goals

Be explicit about what is not being solved immediately:

- no arbitrary C stack freezing;
- no transparent async conversion of every `WaitLatch()`;
- no executor-wide continuation model in the first scheduler phase;
- no third-party C extension compatibility by default in pooled mode;
- no removal of process mode.

### 3. Runtime And Ownership Model

Define what each object owns:

- `PgRuntime`
- `PgCarrier`
- `PgBackend`
- `PgSession`
- `PgConnection`
- `PgExecution`

Important ownership questions:

- Who owns `PGPROC` while a session is idle?
- Is `PGPROC` leased per execution or held for a backend lifetime?
- Who owns `MyLatch` and the interrupt latch at each point?
- What happens to `FeBeWaitSet` when a backend detaches from a carrier?
- Which memory contexts are carrier-local versus backend/session-owned?
- How are error context and resource owner stacks represented at detach
  boundaries?

### 4. Wait Model

Separate wait visibility from scheduler yielding.

Phase 13 currently provides useful wait-completion metadata. It should not be
described as making every wait async.

Classify wait families:

- Top-level frontend input: likely first true scheduler-yielding boundary.
- Frontend output: possible later, but requires resumable flush/backpressure
  handling.
- Heavyweight locks: likely observable first; true yielding requires explicit
  continuation/unwind design.
- Condition variables: observable first; true yielding needs caller-specific
  state.
- LWLocks/semaphores: very sensitive; probably keep carrier-blocking until
  there is a strong reason and a narrow design.
- Checkpoint and auxiliary waits: control-plane correctness first; probably
  not ordinary session scheduling boundaries.

### 5. Scheduler Contract

Specify the scheduler's responsibilities:

- runnable queue;
- waiting queue;
- timeout handling;
- socket readiness handling;
- carrier wakeups;
- backend attach/detach;
- backend exit and carrier exit;
- cancellation and termination delivery.

Key invariant:

```text
A detached logical backend must not have a live C stack on any carrier.
```

### 6. Interrupt And Wakeup Contract

Define how logical and physical wakeups interact:

- logical backend interrupts;
- process signals;
- proc-signal slots;
- latch wakeups;
- wait-completion readiness;
- postmaster child lifecycle events;
- auxiliary process wakeups such as checkpointer, walwriter, bgwriter, and
  autovacuum launcher.

The current blocker in `check-threaded` is likely in this area: a threaded
checkpointer wait is not being woken reliably by the usual checkpoint request
path.

### 7. Safe First Phase

A conservative Phase 14A should probably be:

- pooled runtime object exists;
- scheduler queue and carrier wake mechanics exist;
- only top-level frontend input is a true detach/yield boundary;
- deep waits remain carrier-blocking but visible/cancellable;
- full `check-threaded` and focused pooled tests are green.

This phase should prove the object and ownership model without pretending that
deep PostgreSQL waits are already resumable.

### 8. Later Phases

Possible later phases:

- frontend output would-block/resume handling;
- explicit continuation points for selected lock waits;
- moving `PGPROC` ownership toward execution leasing;
- reducing idle session resource footprint;
- extension gating for pooled scheduler compatibility;
- performance cleanup once correctness is stable.

## Questions For The Next Design Pass

- Is `PGPROC` a property of `PgBackend`, `PgExecution`, or a leased scheduler
  resource?
- What is the smallest safe attach/detach boundary?
- Can `PostgresMain()` be stepped so top-level command read is a clean
  continuation?
- How should frontend protocol state be represented when a backend is parked?
- Which current globals must become session/backend/execution fields before
  pooled scheduling is credible?
- What is the exact lifecycle for `MyLatch`, `FeBeWaitSet`, and interrupt
  latch across carrier attach/detach?
- Which waits must remain carrier-blocking indefinitely?
- How should background workers and auxiliary processes fit into the runtime
  model?
- What validation gate proves Phase 14A is ready?

## Recommended Immediate Work Before More Design Expansion

Before fleshing out a full scheduler design, fix or isolate the current
threaded checkpoint hang described in `MULTITHREADED_CURRENT_STATUS.md`. It is
direct evidence that the ownership/wakeup model is not yet fully understood.

After that, write the full design document against a clean, validated baseline.
