Multithreaded PostgreSQL Benchmarks
===================================

This document records benchmark evidence for the multithreaded PostgreSQL
branch. The latest full benchmark checkpoint is the Phase 16B full-suite and
shell-pool rerun below; the earlier Phase 16 and Phase 15 runs remain
comparison baselines.

Phase 16B full-suite and shell-pool checkpoint
----------------------------------------------

This run repeats the full 12-profile benchmark suite after the Phase 16B
thread-pool hot-path work and then runs a supplemental shell-pool on/off pass
over the same profile list. The two passes answer different questions:

- The full suite compares vanilla PostgreSQL, branch process mode, pinned
  threaded mode, and bounded protocol-carrier pools.
- The shell pass compares pinned threaded mode against
  `threaded_session_pool=shell` with `threaded_session_pool_max=64` and
  `pooled_protocol_carriers=0`. This isolates shell carrier reuse from the
  protocol-carrier scheduler.

| Field | Value |
| --- | --- |
| Date | June 29, 2026 |
| Branch | `phase16-plan` |
| Commit | `5401ee4cd9` |
| Branch install | `/home/sam/codex-work/mtpg-current/tmp_install` |
| Vanilla install | `/home/sam/codex-work/vanilla-pg19/tmp_install` |
| Vanilla identity | `REL_19_BETA1`, `postgres (PostgreSQL) 19beta1` |
| Client install | `/home/sam/codex-work/vanilla-pg19/tmp_install` |
| Full-suite result directory | `/home/sam/codex-work/mtpg-bench-results/phase16b_full_suite_current_20260629_160750` |
| Shell on/off result directory | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_onoff_full_20260629_180959` |
| Full-suite runner | `src/tools/benchmark/mtpg_phase15_benchmark_suite.pl --profiles=all` |
| Shell runner | same suite with `--matrix-arg=--lanes=branch_threaded,branch_shell --matrix-arg=--pool-sizes=64` |
| Result status | Both suites completed with exit status `0`; all recorded rows had `failed_transactions = 0`. |

Validation and environment checks around this benchmark run:

| Check | Result |
| --- | --- |
| `gmake -s -j18` | PASS before the benchmark run |
| `gmake -s install -j18 prefix=/home/sam/codex-work/mtpg-current/tmp_install` | PASS before the benchmark run |
| `perl -c src/tools/benchmark/mtpg_phase15_benchmark_suite.pl` | PASS |
| `perl -c src/tools/benchmark/mtpg_pgbench_matrix.pl` | PASS |
| `git diff --check` | PASS before and after the benchmark/doc pass |
| Filesystem health | `/dev/sdd` on `/` stayed mounted `rw`; about 860 GB remained free after both suites. |

Full-suite performance highlights:

| Profile / workload | Key result |
| --- | --- |
| `pinned_hot` / `builtin_select_prepared` | Vanilla 273165.3 TPS; branch process 236387.3 TPS; pinned threaded 251692.6 TPS. |
| `pinned_hot` / `select1_prepared` | Vanilla 371597.7 TPS; branch process 337934.6 TPS; pinned threaded 356553.0 TPS. |
| `pinned_hot` / `bench_one_prepared` | Vanilla 311695.7 TPS; branch process 284821.0 TPS; pinned threaded 307125.6 TPS. |
| `pinned_hot` / `kv_read_prepared` | Vanilla 266036.8 TPS; branch process 247289.0 TPS; pinned threaded 265700.8 TPS. |
| `pool_realish_100ms` | `branch_pool_64` is 1.006x pinned on indexed read, 1.022x pinned on app transaction, and 1.014x pinned on mixed app work. |
| `pool_realish_1000ms` | `branch_pool_64` is 0.999x pinned on indexed read and 1.000x pinned on app transaction. |
| `pool_idle_100ms` | `branch_pool_64` is 0.999x pinned. |
| `pool_idle_1000ms` | `branch_pool_64` is 0.999x pinned. |
| `pool_scale_1000_realish` | `branch_pool_64` is 0.994x pinned; `branch_pool_512` reaches 1.000x pinned. |
| `pool_scale_1000_idle` | `branch_pool_64` is 0.995x pinned; `branch_pool_512` reaches 0.999x pinned. |
| `pool_stateful_1000ms` | `branch_pool_64` is 0.990x pinned. This remains clean but slightly below pinned. |
| `pool_burst_10ms` | `branch_pool_64` is 0.901x pinned, `branch_pool_128` is 0.964x pinned, and `branch_pool_192` closes the gap at 1.008x pinned. |
| `connection_churn` | Vanilla 3789.8 TPS; branch process 3472.5 TPS; pinned threaded 1613.4 TPS; `branch_pool_64` 2539.4 TPS, or 1.574x pinned. |
| `connection_churn_realish` | Vanilla 2185.2 TPS; branch process 2058.3 TPS; pinned threaded 1369.2 TPS; `branch_pool_64` 1916.4 TPS, or 1.400x pinned. |

Connection-memory profile:

| Lane | Clients | Max server threads | Private delta MB | PSS delta MB | Private KB/client |
| --- | ---: | ---: | ---: | ---: | ---: |
| `vanilla` | 1000 | 1008 | 1159.2 | 1173.3 | 1187.1 |
| `branch_process` | 1000 | 1008 | 1018.4 | 1037.1 | 1042.8 |
| `branch_threaded` | 1000 | 1008 | 1002.2 | 1005.4 | 1026.2 |
| `branch_pool_64` | 1000 | 72 | 575.5 | 578.7 | 589.3 |
| `branch_pool_128` | 1000 | 136 | 601.6 | 604.7 | 616.0 |
| `branch_pool_256` | 1000 | 264 | 660.3 | 663.5 | 676.1 |
| `branch_pool_512` | 1000 | 520 | 780.1 | 783.3 | 798.9 |

Shell-pool on/off pass:

| Profile / workload | Pinned threaded TPS | `branch_shell_64` TPS | Shell / pinned |
| --- | ---: | ---: | ---: |
| `pinned_hot` / `builtin_select_prepared` | 233505.0 | 246838.4 | 1.057 |
| `pinned_hot` / `select1_prepared` | 350585.7 | 353369.8 | 1.008 |
| `pinned_hot` / `bench_one_prepared` | 300773.5 | 302311.4 | 1.005 |
| `pinned_hot` / `kv_read_prepared` | 263280.7 | 262668.9 | 0.998 |
| `pool_realish_100ms` / indexed read | 1958.8 | 1958.9 | 1.000 |
| `pool_realish_100ms` / app transaction | 1808.8 | 1807.2 | 0.999 |
| `pool_realish_100ms` / mixed app work | 1832.2 | 1838.4 | 1.003 |
| `pool_realish_1000ms` / indexed read | 199.5 | 199.5 | 1.000 |
| `pool_realish_1000ms` / app transaction | 197.0 | 196.7 | 0.998 |
| `pool_stateful_1000ms` | 99.6 | 99.6 | 1.000 |
| `pool_scale_1000_realish` | 994.7 | 993.9 | 0.999 |
| `pool_idle_100ms` | 1964.1 | 1962.1 | 0.999 |
| `pool_idle_1000ms` | 199.5 | 199.6 | 1.000 |
| `pool_burst_10ms` | 19148.0 | 19127.5 | 0.999 |
| `pool_scale_1000_idle` | 997.3 | 997.3 | 1.000 |
| `connection_memory_idle` | 995.7 | 996.0 | 1.000 |
| `connection_churn_realish` | 1445.2 | 1996.6 | 1.382 |
| `connection_churn` | 1617.2 | 2648.3 | 1.638 |

Shell-pool memory profile:

| Lane | Clients | Max server threads | Private delta MB | PSS delta MB | Private KB/client |
| --- | ---: | ---: | ---: | ---: | ---: |
| `branch_threaded` | 1000 | 1008 | 998.8 | 1002.0 | 1022.8 |
| `branch_shell_64` | 1000 | 1008 | 1002.8 | 1006.0 | 1026.8 |

Phase 16B benchmark decisions:

- Use the normal protocol carrier pool for steady-state mostly-idle and
  memory-footprint wins. It now matches pinned threaded throughput on the
  100 ms, 1000 ms, and 1000-client idle/real-ish profiles within benchmark
  noise while cutting server-thread and memory footprint substantially.
- Use the shell pool as the reconnect-heavy accelerator. It improves
  connection churn by 1.38x to 1.64x over pinned threaded mode and is neutral
  on hot, idle, stateful, and burst profiles in the full on/off pass.
- Keep shell pooling conceptually separate from the protocol carrier pool.
  `branch_shell_64` keeps one server thread per client in the 1000-connection
  memory profile, so it is not a memory-footprint replacement for
  `branch_pool_N`.
- Do not jump to a full warm backend/session pool based on these numbers. The
  current evidence says the normal thread pool and shell pool cover their
  intended shapes; dirty SQL session reuse should remain fail-closed and
  deferred until a separate benchmark proves it is needed.
- The remaining scheduler target is the short-idle burst profile. Protocol
  pool64 and pool128 are still below pinned threaded at 10 ms sleeps, while
  pool192 closes the gap. Treat this as a capacity/scheduling follow-up rather
  than evidence that the overall pooled path is unhealthy.
- The full-suite checkpoint used tuned glibc malloc, not tcmalloc. Earlier
  tcmalloc probes in preserved result directories were mixed and noisy across
  code points; they did not show a reliable advantage over the tuned glibc
  configuration sufficient to change the default allocator decision.

Phase 16 full-suite rerun
-------------------------

This run repeats the full 12-profile Phase 15 benchmark suite after Phase 16
threaded-world closure work. It uses a fresh non-cassert benchmark install from
the Phase 16 branch so the numbers are not taken from the richer correctness
build.

| Field | Value |
| --- | --- |
| Date | June 27, 2026 |
| Branch | `phase16-plan` |
| Commit | `7ad19683e9` |
| Branch install | `/home/sam/codex-work/mtpg-bench-results/phase16_full_rerun_20260627_205800_branch_install` |
| Vanilla install | `/home/sam/codex-work/vanilla-pg19/tmp_install` |
| Client install | `/home/sam/codex-work/vanilla-pg19/tmp_install` |
| Result directory | `/home/sam/codex-work/mtpg-bench-results/phase16_full_rerun_20260627_205800` |
| Suite index | `/home/sam/codex-work/mtpg-bench-results/phase16_full_rerun_20260627_205800/index.md` |
| Runner | `src/tools/benchmark/mtpg_phase15_benchmark_suite.pl --profiles=all` |
| Comparison baseline | `/home/sam/codex-work/mtpg-bench-results/full_phase15_fixed_20260622_192912` |
| Comparison reports | `tps_compare_vs_full_phase15_fixed.tsv`, `ratio_compare_vs_full_phase15_fixed.tsv`, `memory_compare_vs_full_phase15_fixed.tsv` in the result directory |
| Result status | 12 profiles completed, 101 TPS rows, all `failed_transactions = 0`, suite exit status `0` |

Validation evidence around this benchmark run:

| Target | Result |
| --- | --- |
| `gmake check-threaded-world-coverage` | PASS: 160 covered, 3 excluded, 161 enabled leaves |
| `PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3 gmake check-phase16-gate-g-local` | PASS after installing missing WSL runtime dependencies for Kerberos/OpenLDAP, Tcl, and XSLT. |

Performance classification versus the Phase 15 full-suite baseline:

- No broad throughput regression showed up in the mostly-idle, 100 ms, 1000 ms,
  stateful, burst, or 1000-client pool profiles. Completed rows stayed at
  `failed_transactions = 0`.
- Hot tiny-query absolute branch TPS did not regress. The `pinned_hot`
  `builtin_select_prepared` ratios are lower against the current vanilla run
  (`branch_process` 0.836x and `branch_threaded` 0.739x), but the absolute
  branch TPS is slightly higher than the Phase 15 baseline; the ratio drop is
  mostly because the current vanilla row is much faster.
- Connection churn is the clear regression. `connection_churn`
  `branch_threaded` fell from 1894.9 TPS to 1504.0 TPS, and the
  ratio-to-vanilla fell from 0.518x to 0.398x. `branch_pool_64` fell from
  2077.2 TPS to 1830.1 TPS, and `branch_pool_128` fell from 2092.9 TPS to
  1802.7 TPS. The real-ish churn profile shows the same direction:
  `branch_threaded` fell from 1430.4 TPS to 1178.9 TPS.
- The 1000-client memory profile still shows the intended memory win, but the
  pooled per-client footprint is higher than the Phase 15 baseline. For
  `connection_memory_idle`, `branch_pool_128` PSS/client rose from 560.9 KB to
  616.9 KB, while still remaining about 0.53x vanilla. Protocol park memory's
  median top context rose from about 311 KB to about 354 KB. Treat this as a
  Phase 16 follow-up item rather than a benchmark-run failure.

Phase 16 focused Gate G baseline
--------------------------------

This focused run checks the three performance lanes required for Phase 16
closeout: process mode, thread-per-session mode, and pooled protocol mode. It
is not a replacement for the full 12-profile Phase 15 suite, but it gives a
post-Phase-16 baseline for hot-path overhead, mostly-idle pooled sessions, and
stateful pooled session parking.

| Field | Value |
| --- | --- |
| Date | June 26, 2026 |
| Branch | `phase16-plan` |
| Commit | `a3e3d8be4eac` |
| Branch install | `/tmp/phase16-branch-install` |
| Vanilla install | `/home/sam/codex-work/vanilla-pg19/tmp_install` |
| Client install | `/home/sam/codex-work/vanilla-pg19/tmp_install` |
| Focused result directory | `/home/sam/codex-work/mtpg-bench-results/phase16_gate_g_focused_20260626_174004` |
| Stateful rerun directory | `/home/sam/codex-work/mtpg-bench-results/phase16_gate_g_stateful_20260626_174432` |
| Focused runner | `src/tools/benchmark/mtpg_phase15_benchmark_suite.pl --profiles=pinned_hot,pool_idle_100ms,pool_stateful_1000ms --quick` |
| Stateful rerun | `src/tools/benchmark/mtpg_phase15_benchmark_suite.pl --profiles=pool_stateful_1000ms` |
| Result status | All recorded rows had `failed_transactions = 0` |

Focused hot-path sample:

| Workload | Vanilla TPS | Branch process TPS | Process / vanilla | Branch threaded TPS | Threaded / vanilla |
| --- | ---: | ---: | ---: | ---: | ---: |
| `builtin_select_prepared` | 270840.3 | 247229.8 | 0.913 | 211557.2 | 0.781 |
| `select1_prepared` | 400158.3 | 348827.8 | 0.872 | 355526.8 | 0.888 |
| `bench_one_prepared` | 336945.3 | 295653.7 | 0.877 | 246822.7 | 0.733 |
| `kv_read_prepared` | 282869.1 | 249558.5 | 0.882 | 217557.0 | 0.769 |

Focused 200-client, 100 ms sleep/wake pooled sample:

| Lane | TPS | Ratio to vanilla | Max server processes | Max server threads | Max PSS KB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `vanilla` | 1982.9 | 1.000 | 208 | 208 | 320875 |
| `branch_process` | 1979.5 | 0.998 | 208 | 208 | 294993 |
| `branch_threaded` | 1982.7 | 1.000 | 1 | 208 | 237783 |
| `branch_pool_32` | 1909.8 | 0.963 | 1 | 40 | 213099 |
| `branch_pool_64` | 1940.0 | 0.978 | 1 | 65 | 221437 |
| `branch_pool_128` | 1940.7 | 0.979 | 1 | 64 | 218145 |
| `branch_pool_192` | 1936.2 | 0.976 | 1 | 68 | 221518 |

Normal-duration stateful pooled rerun:

| Lane | TPS | Ratio to vanilla | Max server processes | Max server threads | Max PSS KB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `vanilla` | 99.7 | 1.000 | 109 | 109 | 261644 |
| `branch_process` | 99.7 | 0.999 | 109 | 109 | 212148 |
| `branch_threaded` | 99.7 | 1.000 | 1 | 109 | 207243 |
| `branch_pool_16` | 98.2 | 0.985 | 1 | 24 | 176385 |
| `branch_pool_32` | 98.8 | 0.990 | 1 | 41 | 181833 |
| `branch_pool_64` | 99.0 | 0.993 | 1 | 51 | 186651 |

Performance classification:

- Hot tiny-query overhead remains in the known hot-path bucket. The focused
  Phase 16 sample is not worse than the Phase 15 full-suite shape, but it is
  still behind vanilla on several tiny-query workloads.
- Mostly-idle pooled throughput remains close to process/threaded throughput
  while using materially fewer server threads.
- The short 5-second stateful quick sample under-shot the older baseline for
  pooled lanes, so it was rerun with the normal 30-second, two-run profile. The
  normal run restored the expected 0.985x to 0.993x vanilla range, so the short
  sample is classified as warmup/short-run noise rather than a Phase 16
  regression.

Run metadata
------------

| Field | Value |
| --- | --- |
| Date | June 22, 2026 |
| Branch | `phase15-real-carrier-pool` |
| Commit | `5782dccf5c` |
| Result directory | `/home/sam/codex-work/mtpg-bench-results/full_phase15_fixed_20260622_192912` |
| Suite log | `/home/sam/codex-work/mtpg-bench-results/full_phase15_fixed_20260622_192912.log` |
| Suite index | `/home/sam/codex-work/mtpg-bench-results/full_phase15_fixed_20260622_192912/index.md` |
| Runner | `src/tools/benchmark/mtpg_phase15_benchmark_suite.pl --profiles=all` |
| Matrix runner | `src/tools/benchmark/mtpg_pgbench_matrix.pl` |
| Branch install | `/home/sam/codex-work/mtpg-current/tmp_install` |
| Vanilla install | `/home/sam/codex-work/vanilla-pg19/tmp_install` |
| Client install | `/home/sam/codex-work/vanilla-pg19/tmp_install` |
| Result status | 12 profiles completed, 101 TPS rows, all `failed_transactions = 0` |

Post-benchmark validation was rerun after this benchmark pass:

| Target | Result |
| --- | --- |
| `make check` | PASS |
| `make check-threaded` | PASS |
| `make check-threaded-smoke` | PASS |
| `make check-threaded-150` | PASS |
| `make check-threaded-200` | PASS |
| `make check-threaded-world-core` | PASS |

`check-world` is not a current green target for this branch and was not part of
this validation baseline.

Lane definitions
----------------

| Lane | Meaning |
| --- | --- |
| `vanilla` | PostgreSQL 19 beta 1 built from the vanilla source tree in this workspace. |
| `branch_process` | This branch with normal process-per-backend execution. |
| `branch_threaded` | This branch with `multithreaded = on` and `pooled_protocol_carriers = 0`, giving one carrier thread per session. |
| `branch_pool_N` | This branch with `multithreaded = on` and `pooled_protocol_carriers = N`, giving a bounded pool of protocol carrier threads. |
| `branch_shell_N` | This branch with `multithreaded = on`, `pooled_protocol_carriers = 0`, `threaded_session_pool = shell`, and `threaded_session_pool_max = N`, giving reusable shell carrier threads for reconnect-heavy threaded workloads. |

The pooled mode in this phase only detaches at top-level frontend protocol
input. Deep waits remain carrier-pinned.

Profile definitions
-------------------

| Profile | Clients | Pgbench threads | Runs | Duration | Pool sizes | Purpose |
| --- | ---: | ---: | ---: | ---: | --- | --- |
| `pinned_hot` | 32 | 8 | 3 | 20s | none | Hot-path parity check for vanilla, branch process, and pinned threads. |
| `pool_realish_100ms` | 200 | 32 | 3 | 20s | 32, 64, 128, 192 | Mostly-idle clients with indexed reads, writes, WAL, range reads, and 100 ms client think time. |
| `pool_realish_1000ms` | 200 | 32 | 3 | 25s | 16, 32, 64, 128 | Long-idle clients with table/index work and protocol-read parks. |
| `pool_stateful_1000ms` | 100 | 16 | 2 | 30s | 16, 32, 64 | Stateful temp-table diagnostic, verifying session state survives parking and wakeup. |
| `pool_scale_1000_realish` | 1000 | 64 | 2 | 20s | 64, 128, 256, 512 | Large indexed-read idle population comparing vanilla, pinned threads, and bounded pools. |
| `connection_churn_realish` | 64 | 16 | 3 | 20s | 32, 64, 128 | One database-touching transaction per connection. |
| `pool_idle_100ms` | 200 | 32 | 3 | 15s | 32, 64, 128, 192 | Mostly-idle `SELECT 1` wake cycle at 100 ms. |
| `pool_idle_1000ms` | 200 | 32 | 3 | 20s | 16, 32, 64, 128 | Long-idle `SELECT 1` wake cycle at 1000 ms. |
| `pool_burst_10ms` | 200 | 32 | 3 | 15s | 64, 128, 192 | Short-idle diagnostic for parking overhead and bursty wakeups. |
| `pool_scale_1000_idle` | 1000 | 64 | 3 | 30s | 64, 128, 256, 512 | Large mostly-idle connection population, pinned threads versus pools. |
| `connection_memory_idle` | 1000 | 64 | 2 | 20s | 64, 128, 256, 512 | Large idle connection memory profile with memory detail and protocol-park memory logging. |
| `connection_churn` | 64 | 16 | 3 | 15s | 32, 64, 128 | One tiny transaction per connection. |

Workload definitions
--------------------

All non-builtin SQL workloads use prepared mode. Extra setup creates
`bench_one`, `bench_kv`, and `bench_client_state`, initializes normal
`pgbench_accounts`, vacuums the tables, and checkpoints before measurement.

| Workload | What it does |
| --- | --- |
| `builtin_select_prepared` | Pgbench built-in select-only workload in prepared mode. |
| `select1_prepared` | `SELECT 1;` |
| `bench_one_prepared` | Reads one fixed row from `bench_one`. |
| `kv_read_prepared` | Random point read from `bench_kv`. |
| `select1_sleep_wake_10ms_prepared` | `SELECT 1;`, client-side `\sleep 10 ms`, then `SELECT 1;`. |
| `select1_sleep_wake_100ms_prepared` | `SELECT 1;`, client-side `\sleep 100 ms`, then `SELECT 1;`. |
| `select1_sleep_wake_1000ms_prepared` | `SELECT 1;`, client-side `\sleep 1000 ms`, then `SELECT 1;`. |
| `select1_connect_prepared` | `SELECT 1;` with pgbench reconnecting for each transaction. |
| `kv_read_sleep_wake_100ms_prepared` | Zipfian point read from `bench_kv`, 100 ms client sleep, then another point read of the same key. |
| `kv_read_sleep_wake_1000ms_prepared` | Zipfian point read from `bench_kv`, 1000 ms client sleep, then another point read of the same key. |
| `app_txn_sleep_wake_100ms_prepared` | Account point read, 100 ms client sleep, transaction updating `pgbench_accounts` and `bench_client_state`, commit, then account point read. |
| `app_txn_sleep_wake_1000ms_prepared` | Same as `app_txn_sleep_wake_100ms_prepared`, but with 1000 ms client sleep. |
| `app_mixed_sleep_wake_100ms_prepared` | Zipfian `bench_kv` point read, 50 ms client sleep, small state update, 50 ms client sleep, then range aggregate over nearby `bench_kv` rows. |
| `stateful_temp_sleep_wake_1000ms_prepared` | Sets `application_name`, creates or reuses a session temp table, updates it, sleeps for 1000 ms, then reads it back. |
| `app_txn_connect_prepared` | Reconnect-heavy app transaction: point read and update in `pgbench_accounts`, update of `bench_client_state`, then commit. |

Headline results
----------------

| Signal | Result |
| --- | --- |
| Hot tiny-query path | Branch process is 0.907x to 0.934x vanilla; pinned threads are 0.797x to 0.952x vanilla depending on workload. |
| 200 mostly-idle clients, 100 ms `SELECT 1` wake cycle | `branch_pool_64` and larger are within about 0.4% of vanilla/process/threaded throughput. |
| 200 real-ish clients, 100 ms wake cycle | Pool sizes 64 and larger are close to process/threaded on `kv_read` and `app_txn`; pool 32 has a large `app_mixed` outlier. |
| 1000 mostly-idle clients | `branch_pool_128` reaches 978 TPS versus 997 TPS for pinned threads while using 122 server threads instead of 1008. |
| 1000 mostly-idle memory profile | Pooled lanes use about 537 KB to 561 KB PSS per client versus 961 KB for pinned threads, 1062 KB for branch process, and 1212 KB for vanilla. |
| 1000 real-ish idle clients | `branch_pool_512` reaches 934 TPS, about 0.941x vanilla and 0.948x pinned threads, while using 123 server threads instead of about 1000. |
| Connection churn | Pooled mode is still slower than process and pinned threads. This is not the current design win and remains an optimization target. |

Full TPS results
----------------

The suite records median TPS and latency across each profile's measured runs.

| Profile | Lane | Workload | TPS | Latency ms | Failed txns |
| --- | --- | --- | ---: | ---: | ---: |
| `connection_churn` | `vanilla` | `select1_connect_prepared` | 3655.681716 | 17.507 | 0 |
| `connection_churn` | `branch_process` | `select1_connect_prepared` | 3266.792957 | 19.591 | 0 |
| `connection_churn` | `branch_threaded` | `select1_connect_prepared` | 1894.912628 | 33.775 | 0 |
| `connection_churn` | `branch_pool_32` | `select1_connect_prepared` | 1583.679950 | 40.412 | 0 |
| `connection_churn` | `branch_pool_64` | `select1_connect_prepared` | 2077.182975 | 30.811 | 0 |
| `connection_churn` | `branch_pool_128` | `select1_connect_prepared` | 2092.927361 | 30.579 | 0 |
| `connection_churn_realish` | `vanilla` | `app_txn_connect_prepared` | 2246.298257 | 28.491 | 0 |
| `connection_churn_realish` | `branch_process` | `app_txn_connect_prepared` | 2089.208378 | 30.634 | 0 |
| `connection_churn_realish` | `branch_threaded` | `app_txn_connect_prepared` | 1430.441259 | 44.741 | 0 |
| `connection_churn_realish` | `branch_pool_32` | `app_txn_connect_prepared` | 740.364616 | 86.444 | 0 |
| `connection_churn_realish` | `branch_pool_64` | `app_txn_connect_prepared` | 1143.404354 | 55.973 | 0 |
| `connection_churn_realish` | `branch_pool_128` | `app_txn_connect_prepared` | 1171.831855 | 54.615 | 0 |
| `connection_memory_idle` | `vanilla` | `select1_sleep_wake_1000ms_prepared` | 994.491644 | 1005.539 | 0 |
| `connection_memory_idle` | `branch_process` | `select1_sleep_wake_1000ms_prepared` | 993.473204 | 1006.5695 | 0 |
| `connection_memory_idle` | `branch_threaded` | `select1_sleep_wake_1000ms_prepared` | 990.4881155 | 1009.617 | 0 |
| `connection_memory_idle` | `branch_pool_64` | `select1_sleep_wake_1000ms_prepared` | 942.9938925 | 1060.457 | 0 |
| `connection_memory_idle` | `branch_pool_128` | `select1_sleep_wake_1000ms_prepared` | 957.74689 | 1044.178 | 0 |
| `connection_memory_idle` | `branch_pool_256` | `select1_sleep_wake_1000ms_prepared` | 955.7442005 | 1046.415 | 0 |
| `connection_memory_idle` | `branch_pool_512` | `select1_sleep_wake_1000ms_prepared` | 957.3513345 | 1044.6315 | 0 |
| `pinned_hot` | `vanilla` | `builtin_select_prepared` | 241311.871145 | 0.133 | 0 |
| `pinned_hot` | `vanilla` | `select1_prepared` | 353507.430340 | 0.091 | 0 |
| `pinned_hot` | `vanilla` | `bench_one_prepared` | 294319.405768 | 0.109 | 0 |
| `pinned_hot` | `vanilla` | `kv_read_prepared` | 253972.629926 | 0.126 | 0 |
| `pinned_hot` | `branch_process` | `builtin_select_prepared` | 224819.472158 | 0.142 | 0 |
| `pinned_hot` | `branch_process` | `select1_prepared` | 320657.287165 | 0.100 | 0 |
| `pinned_hot` | `branch_process` | `bench_one_prepared` | 274826.913089 | 0.116 | 0 |
| `pinned_hot` | `branch_process` | `kv_read_prepared` | 233939.343979 | 0.137 | 0 |
| `pinned_hot` | `branch_threaded` | `builtin_select_prepared` | 199645.561364 | 0.160 | 0 |
| `pinned_hot` | `branch_threaded` | `select1_prepared` | 336603.136083 | 0.095 | 0 |
| `pinned_hot` | `branch_threaded` | `bench_one_prepared` | 234590.418958 | 0.136 | 0 |
| `pinned_hot` | `branch_threaded` | `kv_read_prepared` | 207120.210193 | 0.154 | 0 |
| `pool_burst_10ms` | `branch_threaded` | `select1_sleep_wake_10ms_prepared` | 19480.822335 | 10.267 | 0 |
| `pool_burst_10ms` | `branch_pool_64` | `select1_sleep_wake_10ms_prepared` | 6091.409324 | 32.833 | 0 |
| `pool_burst_10ms` | `branch_pool_128` | `select1_sleep_wake_10ms_prepared` | 6193.077420 | 32.294 | 0 |
| `pool_burst_10ms` | `branch_pool_192` | `select1_sleep_wake_10ms_prepared` | 6089.903329 | 32.841 | 0 |
| `pool_idle_1000ms` | `vanilla` | `select1_sleep_wake_1000ms_prepared` | 199.708605 | 1001.459 | 0 |
| `pool_idle_1000ms` | `branch_process` | `select1_sleep_wake_1000ms_prepared` | 199.675328 | 1001.626 | 0 |
| `pool_idle_1000ms` | `branch_threaded` | `select1_sleep_wake_1000ms_prepared` | 199.677361 | 1001.616 | 0 |
| `pool_idle_1000ms` | `branch_pool_16` | `select1_sleep_wake_1000ms_prepared` | 196.035015 | 1020.226 | 0 |
| `pool_idle_1000ms` | `branch_pool_32` | `select1_sleep_wake_1000ms_prepared` | 198.081600 | 1009.685 | 0 |
| `pool_idle_1000ms` | `branch_pool_64` | `select1_sleep_wake_1000ms_prepared` | 199.015609 | 1004.946 | 0 |
| `pool_idle_1000ms` | `branch_pool_128` | `select1_sleep_wake_1000ms_prepared` | 198.998807 | 1005.031 | 0 |
| `pool_idle_100ms` | `vanilla` | `select1_sleep_wake_100ms_prepared` | 1984.743624 | 100.769 | 0 |
| `pool_idle_100ms` | `branch_process` | `select1_sleep_wake_100ms_prepared` | 1982.803938 | 100.867 | 0 |
| `pool_idle_100ms` | `branch_threaded` | `select1_sleep_wake_100ms_prepared` | 1984.259460 | 100.793 | 0 |
| `pool_idle_100ms` | `branch_pool_32` | `select1_sleep_wake_100ms_prepared` | 1958.555654 | 102.116 | 0 |
| `pool_idle_100ms` | `branch_pool_64` | `select1_sleep_wake_100ms_prepared` | 1977.650544 | 101.130 | 0 |
| `pool_idle_100ms` | `branch_pool_128` | `select1_sleep_wake_100ms_prepared` | 1977.482091 | 101.139 | 0 |
| `pool_idle_100ms` | `branch_pool_192` | `select1_sleep_wake_100ms_prepared` | 1977.653319 | 101.130 | 0 |
| `pool_realish_1000ms` | `vanilla` | `kv_read_sleep_wake_1000ms_prepared` | 199.691190 | 1001.546 | 0 |
| `pool_realish_1000ms` | `vanilla` | `app_txn_sleep_wake_1000ms_prepared` | 196.891415 | 1015.788 | 0 |
| `pool_realish_1000ms` | `branch_process` | `kv_read_sleep_wake_1000ms_prepared` | 199.644896 | 1001.779 | 0 |
| `pool_realish_1000ms` | `branch_process` | `app_txn_sleep_wake_1000ms_prepared` | 196.817926 | 1016.168 | 0 |
| `pool_realish_1000ms` | `branch_threaded` | `kv_read_sleep_wake_1000ms_prepared` | 199.549728 | 1002.256 | 0 |
| `pool_realish_1000ms` | `branch_threaded` | `app_txn_sleep_wake_1000ms_prepared` | 196.439757 | 1018.124 | 0 |
| `pool_realish_1000ms` | `branch_pool_16` | `kv_read_sleep_wake_1000ms_prepared` | 196.507125 | 1017.775 | 0 |
| `pool_realish_1000ms` | `branch_pool_16` | `app_txn_sleep_wake_1000ms_prepared` | 190.342710 | 1050.736 | 0 |
| `pool_realish_1000ms` | `branch_pool_32` | `kv_read_sleep_wake_1000ms_prepared` | 198.142666 | 1009.374 | 0 |
| `pool_realish_1000ms` | `branch_pool_32` | `app_txn_sleep_wake_1000ms_prepared` | 194.989199 | 1025.698 | 0 |
| `pool_realish_1000ms` | `branch_pool_64` | `kv_read_sleep_wake_1000ms_prepared` | 198.774729 | 1006.164 | 0 |
| `pool_realish_1000ms` | `branch_pool_64` | `app_txn_sleep_wake_1000ms_prepared` | 196.318810 | 1018.751 | 0 |
| `pool_realish_1000ms` | `branch_pool_128` | `kv_read_sleep_wake_1000ms_prepared` | 198.794423 | 1006.064 | 0 |
| `pool_realish_1000ms` | `branch_pool_128` | `app_txn_sleep_wake_1000ms_prepared` | 196.191630 | 1019.411 | 0 |
| `pool_realish_100ms` | `vanilla` | `kv_read_sleep_wake_100ms_prepared` | 1983.744317 | 100.819 | 0 |
| `pool_realish_100ms` | `vanilla` | `app_txn_sleep_wake_100ms_prepared` | 1816.717162 | 110.089 | 0 |
| `pool_realish_100ms` | `vanilla` | `app_mixed_sleep_wake_100ms_prepared` | 1836.323210 | 108.913 | 0 |
| `pool_realish_100ms` | `branch_process` | `kv_read_sleep_wake_100ms_prepared` | 1979.825082 | 101.019 | 0 |
| `pool_realish_100ms` | `branch_process` | `app_txn_sleep_wake_100ms_prepared` | 1814.238489 | 110.239 | 0 |
| `pool_realish_100ms` | `branch_process` | `app_mixed_sleep_wake_100ms_prepared` | 1834.903801 | 108.998 | 0 |
| `pool_realish_100ms` | `branch_threaded` | `kv_read_sleep_wake_100ms_prepared` | 1978.517220 | 101.086 | 0 |
| `pool_realish_100ms` | `branch_threaded` | `app_txn_sleep_wake_100ms_prepared` | 1819.091188 | 109.945 | 0 |
| `pool_realish_100ms` | `branch_threaded` | `app_mixed_sleep_wake_100ms_prepared` | 1841.325534 | 108.617 | 0 |
| `pool_realish_100ms` | `branch_pool_32` | `kv_read_sleep_wake_100ms_prepared` | 1963.124318 | 101.878 | 0 |
| `pool_realish_100ms` | `branch_pool_32` | `app_txn_sleep_wake_100ms_prepared` | 1761.253905 | 113.555 | 0 |
| `pool_realish_100ms` | `branch_pool_32` | `app_mixed_sleep_wake_100ms_prepared` | 1219.956758 | 163.940 | 0 |
| `pool_realish_100ms` | `branch_pool_64` | `kv_read_sleep_wake_100ms_prepared` | 1974.099514 | 101.312 | 0 |
| `pool_realish_100ms` | `branch_pool_64` | `app_txn_sleep_wake_100ms_prepared` | 1861.570141 | 107.436 | 0 |
| `pool_realish_100ms` | `branch_pool_64` | `app_mixed_sleep_wake_100ms_prepared` | 1829.443023 | 109.323 | 0 |
| `pool_realish_100ms` | `branch_pool_128` | `kv_read_sleep_wake_100ms_prepared` | 1972.988486 | 101.369 | 0 |
| `pool_realish_100ms` | `branch_pool_128` | `app_txn_sleep_wake_100ms_prepared` | 1862.348660 | 107.391 | 0 |
| `pool_realish_100ms` | `branch_pool_128` | `app_mixed_sleep_wake_100ms_prepared` | 1821.892538 | 109.776 | 0 |
| `pool_realish_100ms` | `branch_pool_192` | `kv_read_sleep_wake_100ms_prepared` | 1974.387583 | 101.297 | 0 |
| `pool_realish_100ms` | `branch_pool_192` | `app_txn_sleep_wake_100ms_prepared` | 1854.851747 | 107.825 | 0 |
| `pool_realish_100ms` | `branch_pool_192` | `app_mixed_sleep_wake_100ms_prepared` | 1829.088663 | 109.344 | 0 |
| `pool_scale_1000_idle` | `branch_threaded` | `select1_sleep_wake_1000ms_prepared` | 996.639398 | 1003.372 | 0 |
| `pool_scale_1000_idle` | `branch_pool_64` | `select1_sleep_wake_1000ms_prepared` | 961.133350 | 1040.438 | 0 |
| `pool_scale_1000_idle` | `branch_pool_128` | `select1_sleep_wake_1000ms_prepared` | 978.117700 | 1022.372 | 0 |
| `pool_scale_1000_idle` | `branch_pool_256` | `select1_sleep_wake_1000ms_prepared` | 977.409270 | 1023.113 | 0 |
| `pool_scale_1000_idle` | `branch_pool_512` | `select1_sleep_wake_1000ms_prepared` | 978.006237 | 1022.488 | 0 |
| `pool_scale_1000_realish` | `vanilla` | `kv_read_sleep_wake_1000ms_prepared` | 992.918038 | 1007.1325 | 0 |
| `pool_scale_1000_realish` | `branch_threaded` | `kv_read_sleep_wake_1000ms_prepared` | 985.155781 | 1015.069 | 0 |
| `pool_scale_1000_realish` | `branch_pool_64` | `kv_read_sleep_wake_1000ms_prepared` | 932.1103975 | 1072.839 | 0 |
| `pool_scale_1000_realish` | `branch_pool_128` | `kv_read_sleep_wake_1000ms_prepared` | 929.219954 | 1076.195 | 0 |
| `pool_scale_1000_realish` | `branch_pool_256` | `kv_read_sleep_wake_1000ms_prepared` | 931.696427 | 1073.313 | 0 |
| `pool_scale_1000_realish` | `branch_pool_512` | `kv_read_sleep_wake_1000ms_prepared` | 934.017647 | 1070.6495 | 0 |
| `pool_stateful_1000ms` | `vanilla` | `stateful_temp_sleep_wake_1000ms_prepared` | 99.7167815 | 1002.84 | 0 |
| `pool_stateful_1000ms` | `branch_process` | `stateful_temp_sleep_wake_1000ms_prepared` | 99.693457 | 1003.075 | 0 |
| `pool_stateful_1000ms` | `branch_threaded` | `stateful_temp_sleep_wake_1000ms_prepared` | 99.7014335 | 1002.9945 | 0 |
| `pool_stateful_1000ms` | `branch_pool_16` | `stateful_temp_sleep_wake_1000ms_prepared` | 98.21898 | 1018.1335 | 0 |
| `pool_stateful_1000ms` | `branch_pool_32` | `stateful_temp_sleep_wake_1000ms_prepared` | 98.9102855 | 1011.018 | 0 |
| `pool_stateful_1000ms` | `branch_pool_64` | `stateful_temp_sleep_wake_1000ms_prepared` | 99.0306455 | 1009.789 | 0 |

Memory footprint results
------------------------

The table below records the comparable columns from each profile's
`memory_footprint.tsv`. The raw output directory contains the full RSS, PSS,
shared, private, sampled process/thread, map, and protocol-park attribution
files.

`Pooled idle PSS KB` and `Pooled carrier PSS KB` are fitted estimates derived
from the pool-size sweep for that profile. Negative fitted carrier values in
the simple connection-churn profile indicate that this profile is too transient
for the pooled linear fit to be meaningful.

| Profile | Workload | Lane | Clients | Max proc | Max threads | PSS/client KB | Private/client KB | Pooled idle PSS KB | Pooled carrier PSS KB |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `connection_churn` | `select1_connect_prepared` | `vanilla` | 64 | 58 | 39 | 372.45 | 357.56 | 1672.20 | -1144.91 |
| `connection_churn` | `select1_connect_prepared` | `branch_process` | 64 | 61 | 44 | 355.92 | 347.25 | 1672.20 | -1144.91 |
| `connection_churn` | `select1_connect_prepared` | `branch_threaded` | 64 | 1 | 66 | 523.39 | 521.81 | 1672.20 | -1144.91 |
| `connection_churn` | `select1_connect_prepared` | `branch_pool_32` | 64 | 1 | 41 | 1136.20 | 1133.12 | 1672.20 | -1144.91 |
| `connection_churn` | `select1_connect_prepared` | `branch_pool_64` | 64 | 1 | 66 | 670.64 | 669.19 | 1672.20 | -1144.91 |
| `connection_churn` | `select1_connect_prepared` | `branch_pool_128` | 64 | 1 | 66 | 687.38 | 685.62 | 1672.20 | -1144.91 |
| `connection_churn_realish` | `app_txn_connect_prepared` | `vanilla` | 64 | 73 | 54 | 980.78 | 808.38 | 1085.38 | 238.75 |
| `connection_churn_realish` | `app_txn_connect_prepared` | `branch_process` | 64 | 71 | 53 | 914.17 | 765.31 | 1085.38 | 238.75 |
| `connection_churn_realish` | `app_txn_connect_prepared` | `branch_threaded` | 64 | 1 | 72 | 1030.47 | 1026.38 | 1085.38 | 238.75 |
| `connection_churn_realish` | `app_txn_connect_prepared` | `branch_pool_32` | 64 | 1 | 41 | 1192.88 | 1189.81 | 1085.38 | 238.75 |
| `connection_churn_realish` | `app_txn_connect_prepared` | `branch_pool_64` | 64 | 1 | 73 | 1332.86 | 1330.00 | 1085.38 | 238.75 |
| `connection_churn_realish` | `app_txn_connect_prepared` | `branch_pool_128` | 64 | 1 | 84 | 1349.66 | 1346.56 | 1085.38 | 238.75 |
| `connection_memory_idle` | `select1_sleep_wake_1000ms_prepared` | `vanilla` | 1000 | 1008 | 1008 | 1211.77 | 1197.78 | 505.89 | 483.09 |
| `connection_memory_idle` | `select1_sleep_wake_1000ms_prepared` | `branch_process` | 1000 | 1009 | 1008 | 1061.96 | 1042.69 | 505.89 | 483.09 |
| `connection_memory_idle` | `select1_sleep_wake_1000ms_prepared` | `branch_threaded` | 1000 | 1 | 1008 | 961.12 | 957.84 | 505.89 | 483.09 |
| `connection_memory_idle` | `select1_sleep_wake_1000ms_prepared` | `branch_pool_64` | 1000 | 1 | 72 | 536.70 | 533.41 | 505.89 | 483.09 |
| `connection_memory_idle` | `select1_sleep_wake_1000ms_prepared` | `branch_pool_128` | 1000 | 1 | 121 | 560.90 | 557.63 | 505.89 | 483.09 |
| `connection_memory_idle` | `select1_sleep_wake_1000ms_prepared` | `branch_pool_256` | 1000 | 1 | 122 | 559.85 | 556.56 | 505.89 | 483.09 |
| `connection_memory_idle` | `select1_sleep_wake_1000ms_prepared` | `branch_pool_512` | 1000 | 1 | 116 | 558.87 | 555.61 | 505.89 | 483.09 |
| `pool_burst_10ms` | `select1_sleep_wake_10ms_prepared` | `branch_threaded` | 200 | 1 | 208 | 636.28 | 633.00 | 137.22 | 1518.00 |
| `pool_burst_10ms` | `select1_sleep_wake_10ms_prepared` | `branch_pool_64` | 200 | 1 | 71 | 599.88 | 596.88 | 137.22 | 1518.00 |
| `pool_burst_10ms` | `select1_sleep_wake_10ms_prepared` | `branch_pool_128` | 200 | 1 | 72 | 607.80 | 604.90 | 137.22 | 1518.00 |
| `pool_burst_10ms` | `select1_sleep_wake_10ms_prepared` | `branch_pool_192` | 200 | 1 | 71 | 600.54 | 597.16 | 137.22 | 1518.00 |
| `pool_idle_1000ms` | `select1_sleep_wake_1000ms_prepared` | `vanilla` | 200 | 208 | 208 | 1219.39 | 1182.90 | 545.99 | 228.51 |
| `pool_idle_1000ms` | `select1_sleep_wake_1000ms_prepared` | `branch_process` | 200 | 208 | 208 | 1084.05 | 1045.56 | 545.99 | 228.51 |
| `pool_idle_1000ms` | `select1_sleep_wake_1000ms_prepared` | `branch_threaded` | 200 | 1 | 208 | 656.30 | 652.90 | 545.99 | 228.51 |
| `pool_idle_1000ms` | `select1_sleep_wake_1000ms_prepared` | `branch_pool_16` | 200 | 1 | 24 | 559.65 | 556.30 | 545.99 | 228.51 |
| `pool_idle_1000ms` | `select1_sleep_wake_1000ms_prepared` | `branch_pool_32` | 200 | 1 | 40 | 583.74 | 580.34 | 545.99 | 228.51 |
| `pool_idle_1000ms` | `select1_sleep_wake_1000ms_prepared` | `branch_pool_64` | 200 | 1 | 73 | 615.32 | 611.98 | 545.99 | 228.51 |
| `pool_idle_1000ms` | `select1_sleep_wake_1000ms_prepared` | `branch_pool_128` | 200 | 1 | 72 | 619.50 | 616.10 | 545.99 | 228.51 |
| `pool_idle_100ms` | `select1_sleep_wake_100ms_prepared` | `vanilla` | 200 | 208 | 208 | 1208.63 | 1174.34 | 548.11 | 227.46 |
| `pool_idle_100ms` | `select1_sleep_wake_100ms_prepared` | `branch_process` | 200 | 208 | 208 | 1083.64 | 1044.70 | 548.11 | 227.46 |
| `pool_idle_100ms` | `select1_sleep_wake_100ms_prepared` | `branch_threaded` | 200 | 1 | 208 | 636.88 | 633.62 | 548.11 | 227.46 |
| `pool_idle_100ms` | `select1_sleep_wake_100ms_prepared` | `branch_pool_32` | 200 | 1 | 40 | 582.23 | 578.90 | 548.11 | 227.46 |
| `pool_idle_100ms` | `select1_sleep_wake_100ms_prepared` | `branch_pool_64` | 200 | 1 | 72 | 622.63 | 619.28 | 548.11 | 227.46 |
| `pool_idle_100ms` | `select1_sleep_wake_100ms_prepared` | `branch_pool_128` | 200 | 1 | 72 | 613.18 | 609.92 | 548.11 | 227.46 |
| `pool_idle_100ms` | `select1_sleep_wake_100ms_prepared` | `branch_pool_192` | 200 | 1 | 71 | 620.04 | 616.72 | 548.11 | 227.46 |
| `pool_realish_1000ms` | `kv_read_sleep_wake_1000ms_prepared` | `vanilla` | 200 | 209 | 209 | 1395.29 | 1329.40 | 792.73 | 102.22 |
| `pool_realish_1000ms` | `kv_read_sleep_wake_1000ms_prepared` | `branch_process` | 200 | 208 | 208 | 1276.58 | 1188.22 | 792.73 | 102.22 |
| `pool_realish_1000ms` | `kv_read_sleep_wake_1000ms_prepared` | `branch_threaded` | 200 | 1 | 208 | 818.50 | 815.24 | 792.73 | 102.22 |
| `pool_realish_1000ms` | `kv_read_sleep_wake_1000ms_prepared` | `branch_pool_16` | 200 | 1 | 26 | 806.20 | 802.72 | 792.73 | 102.22 |
| `pool_realish_1000ms` | `kv_read_sleep_wake_1000ms_prepared` | `branch_pool_32` | 200 | 1 | 41 | 799.52 | 796.26 | 792.73 | 102.22 |
| `pool_realish_1000ms` | `kv_read_sleep_wake_1000ms_prepared` | `branch_pool_64` | 200 | 1 | 72 | 835.33 | 831.98 | 792.73 | 102.22 |
| `pool_realish_1000ms` | `kv_read_sleep_wake_1000ms_prepared` | `branch_pool_128` | 200 | 1 | 73 | 817.25 | 813.90 | 792.73 | 102.22 |
| `pool_realish_1000ms` | `app_txn_sleep_wake_1000ms_prepared` | `vanilla` | 200 | 208 | 208 | 1552.86 | 1391.98 | 1017.53 | 188.68 |
| `pool_realish_1000ms` | `app_txn_sleep_wake_1000ms_prepared` | `branch_process` | 200 | 208 | 208 | 1425.51 | 1273.62 | 1017.53 | 188.68 |
| `pool_realish_1000ms` | `app_txn_sleep_wake_1000ms_prepared` | `branch_threaded` | 200 | 1 | 208 | 1018.28 | 1014.88 | 1017.53 | 188.68 |
| `pool_realish_1000ms` | `app_txn_sleep_wake_1000ms_prepared` | `branch_pool_16` | 200 | 1 | 24 | 1027.40 | 1024.00 | 1017.53 | 188.68 |
| `pool_realish_1000ms` | `app_txn_sleep_wake_1000ms_prepared` | `branch_pool_32` | 200 | 1 | 41 | 1050.64 | 1047.24 | 1017.53 | 188.68 |
| `pool_realish_1000ms` | `app_txn_sleep_wake_1000ms_prepared` | `branch_pool_64` | 200 | 1 | 72 | 1071.93 | 1068.54 | 1017.53 | 188.68 |
| `pool_realish_1000ms` | `app_txn_sleep_wake_1000ms_prepared` | `branch_pool_128` | 200 | 1 | 73 | 1079.59 | 1076.52 | 1017.53 | 188.68 |
| `pool_realish_100ms` | `kv_read_sleep_wake_100ms_prepared` | `vanilla` | 200 | 209 | 209 | 1400.52 | 1325.94 | 782.38 | 105.65 |
| `pool_realish_100ms` | `kv_read_sleep_wake_100ms_prepared` | `branch_process` | 200 | 208 | 208 | 1273.08 | 1178.00 | 782.38 | 105.65 |
| `pool_realish_100ms` | `kv_read_sleep_wake_100ms_prepared` | `branch_threaded` | 200 | 1 | 210 | 890.77 | 887.42 | 782.38 | 105.65 |
| `pool_realish_100ms` | `kv_read_sleep_wake_100ms_prepared` | `branch_pool_32` | 200 | 1 | 41 | 798.13 | 794.76 | 782.38 | 105.65 |
| `pool_realish_100ms` | `kv_read_sleep_wake_100ms_prepared` | `branch_pool_64` | 200 | 1 | 72 | 815.08 | 811.64 | 782.38 | 105.65 |
| `pool_realish_100ms` | `kv_read_sleep_wake_100ms_prepared` | `branch_pool_128` | 200 | 1 | 72 | 818.27 | 814.92 | 782.38 | 105.65 |
| `pool_realish_100ms` | `kv_read_sleep_wake_100ms_prepared` | `branch_pool_192` | 200 | 1 | 74 | 812.67 | 809.30 | 782.38 | 105.65 |
| `pool_realish_100ms` | `app_txn_sleep_wake_100ms_prepared` | `vanilla` | 200 | 208 | 208 | 1538.73 | 1389.70 | 1015.25 | 174.63 |
| `pool_realish_100ms` | `app_txn_sleep_wake_100ms_prepared` | `branch_process` | 200 | 208 | 208 | 1431.88 | 1278.08 | 1015.25 | 174.63 |
| `pool_realish_100ms` | `app_txn_sleep_wake_100ms_prepared` | `branch_threaded` | 200 | 1 | 209 | 1109.66 | 1106.26 | 1015.25 | 174.63 |
| `pool_realish_100ms` | `app_txn_sleep_wake_100ms_prepared` | `branch_pool_32` | 200 | 1 | 40 | 1040.60 | 1037.20 | 1015.25 | 174.63 |
| `pool_realish_100ms` | `app_txn_sleep_wake_100ms_prepared` | `branch_pool_64` | 200 | 1 | 73 | 1075.37 | 1071.94 | 1015.25 | 174.63 |
| `pool_realish_100ms` | `app_txn_sleep_wake_100ms_prepared` | `branch_pool_128` | 200 | 1 | 72 | 1068.24 | 1064.82 | 1015.25 | 174.63 |
| `pool_realish_100ms` | `app_txn_sleep_wake_100ms_prepared` | `branch_pool_192` | 200 | 1 | 74 | 1066.24 | 1062.82 | 1015.25 | 174.63 |
| `pool_realish_100ms` | `app_mixed_sleep_wake_100ms_prepared` | `vanilla` | 200 | 208 | 208 | 1609.98 | 1484.60 | 1139.34 | 190.54 |
| `pool_realish_100ms` | `app_mixed_sleep_wake_100ms_prepared` | `branch_process` | 200 | 208 | 208 | 1508.88 | 1368.62 | 1139.34 | 190.54 |
| `pool_realish_100ms` | `app_mixed_sleep_wake_100ms_prepared` | `branch_threaded` | 200 | 1 | 209 | 1240.80 | 1237.42 | 1139.34 | 190.54 |
| `pool_realish_100ms` | `app_mixed_sleep_wake_100ms_prepared` | `branch_pool_32` | 200 | 1 | 40 | 1166.68 | 1163.34 | 1139.34 | 190.54 |
| `pool_realish_100ms` | `app_mixed_sleep_wake_100ms_prepared` | `branch_pool_64` | 200 | 1 | 72 | 1211.26 | 1208.02 | 1139.34 | 190.54 |
| `pool_realish_100ms` | `app_mixed_sleep_wake_100ms_prepared` | `branch_pool_128` | 200 | 1 | 73 | 1189.99 | 1186.42 | 1139.34 | 190.54 |
| `pool_realish_100ms` | `app_mixed_sleep_wake_100ms_prepared` | `branch_pool_192` | 200 | 1 | 73 | 1195.19 | 1191.88 | 1139.34 | 190.54 |
| `pool_scale_1000_idle` | `select1_sleep_wake_1000ms_prepared` | `branch_threaded` | 1000 | 1 | 1008 | 703.64 | 700.41 | 502.89 | 217.08 |
| `pool_scale_1000_idle` | `select1_sleep_wake_1000ms_prepared` | `branch_pool_64` | 1000 | 1 | 72 | 516.35 | 513.22 | 502.89 | 217.08 |
| `pool_scale_1000_idle` | `select1_sleep_wake_1000ms_prepared` | `branch_pool_128` | 1000 | 1 | 122 | 527.21 | 524.01 | 502.89 | 217.08 |
| `pool_scale_1000_idle` | `select1_sleep_wake_1000ms_prepared` | `branch_pool_256` | 1000 | 1 | 124 | 527.87 | 524.67 | 502.89 | 217.08 |
| `pool_scale_1000_idle` | `select1_sleep_wake_1000ms_prepared` | `branch_pool_512` | 1000 | 1 | 124 | 527.40 | 524.20 | 502.89 | 217.08 |
| `pool_scale_1000_realish` | `kv_read_sleep_wake_1000ms_prepared` | `vanilla` | 1000 | 1008 | 1008 | 1404.81 | 1371.92 | 734.01 | 168.07 |
| `pool_scale_1000_realish` | `kv_read_sleep_wake_1000ms_prepared` | `branch_threaded` | 1000 | 1 | 1009 | 920.65 | 917.44 | 734.01 | 168.07 |
| `pool_scale_1000_realish` | `kv_read_sleep_wake_1000ms_prepared` | `branch_pool_64` | 1000 | 1 | 73 | 744.47 | 741.26 | 734.01 | 168.07 |
| `pool_scale_1000_realish` | `kv_read_sleep_wake_1000ms_prepared` | `branch_pool_128` | 1000 | 1 | 121 | 752.24 | 749.02 | 734.01 | 168.07 |
| `pool_scale_1000_realish` | `kv_read_sleep_wake_1000ms_prepared` | `branch_pool_256` | 1000 | 1 | 122 | 752.06 | 748.83 | 734.01 | 168.07 |
| `pool_scale_1000_realish` | `kv_read_sleep_wake_1000ms_prepared` | `branch_pool_512` | 1000 | 1 | 123 | 753.82 | 750.60 | 734.01 | 168.07 |
| `pool_stateful_1000ms` | `stateful_temp_sleep_wake_1000ms_prepared` | `vanilla` | 100 | 109 | 109 | 1741.35 | 1549.32 | 1402.14 | 306.28 |
| `pool_stateful_1000ms` | `stateful_temp_sleep_wake_1000ms_prepared` | `branch_process` | 100 | 108 | 108 | 1688.97 | 1412.00 | 1402.14 | 306.28 |
| `pool_stateful_1000ms` | `stateful_temp_sleep_wake_1000ms_prepared` | `branch_threaded` | 100 | 1 | 108 | 1774.22 | 1768.88 | 1402.14 | 306.28 |
| `pool_stateful_1000ms` | `stateful_temp_sleep_wake_1000ms_prepared` | `branch_pool_16` | 100 | 1 | 25 | 1456.37 | 1451.04 | 1402.14 | 306.28 |
| `pool_stateful_1000ms` | `stateful_temp_sleep_wake_1000ms_prepared` | `branch_pool_32` | 100 | 1 | 41 | 1498.39 | 1493.24 | 1402.14 | 306.28 |
| `pool_stateful_1000ms` | `stateful_temp_sleep_wake_1000ms_prepared` | `branch_pool_64` | 100 | 1 | 54 | 1545.69 | 1540.36 | 1402.14 | 306.28 |

Raw artifact guide
------------------

Each profile directory contains:

| File | Contents |
| --- | --- |
| `tps.tsv` | Median TPS and latency by lane/workload. |
| `samples.tsv` | Per-run TPS and latency samples. |
| `ratios.tsv` | Ratios against vanilla, or the first selected lane when vanilla is absent. |
| `server_resources.tsv` | Max sampled server process/thread counts and aggregate memory by workload. |
| `server_resource_samples.tsv` | Raw sampled process-tree memory observations. |
| `server_resource_baselines.tsv` | Idle server resource samples before workload clients. |
| `resource_efficiency.tsv` | Derived TPS/thread and memory/client metrics. |
| `memory_footprint.tsv` | Baseline-adjusted memory footprint estimates. |
| `server_process_rollups.tsv` | Per-process `smaps_rollup` rows when memory detail is enabled. |
| `server_memory_map_summary.tsv` | One detailed `smaps` category snapshot per run when memory detail is enabled. |
| `server_memory_map_path_summary.tsv` | Detailed `smaps` totals by category and mapped path. |
| `server_thread_stacks.tsv` | Per-thread stack visibility for detailed snapshots. |
| `protocol_park_memory.tsv` | Parsed per-park memory-context attribution rows when protocol park logging is enabled. |
| `protocol_park_guc_memory.tsv` | Parsed per-park GUC memory attribution rows when protocol park logging is enabled. |
| `protocol_park_context_memory.tsv` | Bounded per-backend memory-context tree rows emitted at committed protocol-read parks. |
| `protocol_park_*_summary.tsv` | Median per-park memory summaries by lane/workload. |

Current interpretation
----------------------

Pooled protocol carriers now show the intended shape for large quiet connection
populations: much lower server thread counts and materially lower per-client
PSS than pinned threads, process mode, or vanilla. The strongest current proof
point is the 1000-client idle memory profile, where pooled lanes stay around
943 TPS to 958 TPS and about 537 KB to 561 KB PSS per client, compared with
990 TPS and 961 KB PSS per client for pinned threads.

The branch is not yet back to the earlier hot-path speed position. The hot
tiny-query profile shows branch process and pinned threads behind vanilla, and
the connection-churn profiles show pooled mode behind all non-pooled lanes.
Those remain the next performance targets.

Phase 16B Stage 0-5 warm session pool evidence
===============================================

Run date: 2026-06-28. Branch: `phase16-plan`.

Artifact directories:

| Stage | Directory |
| --- | --- |
| Stage 0 baseline | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage0_baseline_20260628_043249` |
| Stage 0 branch install | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage0_baseline_20260628_043249_branch_install` |
| Stage 1 lifecycle build/install | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage1_lifecycle_20260628_052900` and `/home/sam/codex-work/mtpg-bench-results/phase16b_stage1_lifecycle_20260628_052900_branch_install` |
| Stage 1 instrumentation off | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage1_disabled_20260628_053500` |
| Stage 1 instrumentation on | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage1_enabled_20260628_054300` |
| Stage 2 burst upper-bound proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage2_upper_bound_20260628_055000` |
| Stage 4 shell carrier proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_proof_20260628_061003` |
| Stage 4 shell carrier proof install | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_proof_20260628_061003_branch_install` |
| Stage 4 final shell counter/checkpoint base | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_counters_20260628_062840` |
| Stage 4 final shell install | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_counters_20260628_062840_branch_install` |
| Stage 5 validator scaffold proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_validator_20260628_074210` |
| Stage 5 expanded validator proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_validator_expanded_20260628_074953` |
| Stage 5 shell state-isolation proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_shell_state_isolation_20260628_075754` |
| Stage 5 quarantine destroy proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_quarantine_destroy_20260628_080648` |
| Stage 5 buffer refcount validator proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_buffer_refcount_validator_20260628_081523` |
| Stage 5 pgstat/ipc validator proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_pgstat_ipc_validator_20260628_082241` |
| Stage 5 GUC baseline validator proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_guc_baseline_validator_20260628_083257` |
| Stage 5 socket/FD validator proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_socket_fd_validator_20260628_085124` |
| Stage 5 lock validator proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_lock_validator_20260628_085954` |
| Stage 5 procarray/procsignal validator proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_procarray_procsignal_validator_20260628_090711` |
| Stage 5 real socket/FD no-leak proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_socket_fd_real_no_leak_20260628_091526` |
| Stage 5 real buffer-pin reset proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_buffer_pin_real_reset_20260628_092319` |
| Stage 5 same-key GUC baseline proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_same_key_guc_baseline_20260628_093248` |
| Stage 5 failed-validation no-leak proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_failed_validation_no_leak_20260628_093924` |
| Post-Stage-5 churn checkpoint | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage6_churn_full_20260628_094300` |

Correctness and build gates:

- `PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3 gmake check-phase16-gate-g-local`
  passed before the Phase 16B source changes.
- The Stage 1 benchmark install was rebuilt as a lean non-cassert install with
  `--without-icu --disable-rpath --with-perl`.
- `PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3 gmake check-runtime-lifecycles
  check-global-lifetimes` passed after the lifecycle instrumentation was added.
- `git diff --check` passed after the instrumentation edit and before this
  document update.
- `gmake -j18`, `gmake install`, `PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3
  gmake check-runtime-lifecycles check-global-lifetimes`, and
  `git diff --check` passed after the Stage 5 validator scaffold and
  documentation updates.
- Direct TAP proof
  `prove -I src/test/perl src/test/modules/test_backend_runtime/t/011_phase16b_shell_pool_fallback.pl`
  passed with the Stage 4 branch install on `PATH`. The test configures
  `threaded_session_pool=shell`, `threaded_session_pool_max=2`, admits five
  concurrent clients, and verifies dedicated fallback accounting without
  carrier start failures.
- Initial Stage 5 validator proof passed in
  `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_validator_20260628_074210`.
  The module `check` target rebuilt and installed the extension, ran the
  process-mode SQL regression, and reported TAP disabled for the configured
  tree. Direct `prove` then passed
  `011_phase16b_shell_pool_fallback.pl` and
  `012_phase16b_reusable_session_validator.pl` against the temp install.
- Expanded Stage 5 validator proof passed in
  `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_validator_expanded_20260628_074953`.
  This reran the same module `check` target and direct `prove` pair after
  adding reason-code tests for lock state, buffer pin indicators, storage
  state, temp files, GUC nesting, plan cache state, invalidations, snapshots,
  XLog insert state, and async actions.
- Stage 5 shell state-isolation proof passed in
  `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_shell_state_isolation_20260628_075754`.
  The module `check` target rebuilt and installed the extension, ran the
  process-mode SQL regression, and reported TAP disabled for the configured
  tree. Direct `prove` then passed `011_phase16b_shell_pool_fallback.pl`,
  `012_phase16b_reusable_session_validator.pl`, and
  `013_phase16b_shell_pool_state_isolation.pl` against the temp install. The
  new runtime proof dirties a shell session with a temp table, prepared
  statement, LISTEN state, changed `work_mem`, and a session advisory lock,
  then verifies that subsequent clients do not inherit those states.
- Stage 5 quarantine destroy proof passed in
  `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_quarantine_destroy_20260628_080648`.
  The module `check` target rebuilt and installed the extension, ran the
  process-mode SQL regression, and reported TAP disabled for the configured
  tree. Direct `prove` then passed `011_phase16b_shell_pool_fallback.pl`,
  `012_phase16b_reusable_session_validator.pl`,
  `013_phase16b_shell_pool_state_isolation.pl`, and
  `014_phase16b_shell_pool_quarantine_destroy.pl` against the temp install.
  The new runtime proof enables the hidden
  `debug_threaded_session_pool_force_validation_failure` test knob, forces a
  failed validation when the session is otherwise clean, verifies
  `action=quarantine_destroy` with nonzero `quarantine_destroy_paths`, and
  confirms the failed-validation destroyed session does not leak temp table,
  prepared statement, LISTEN, GUC, or advisory-lock state to later clients.
- Stage 5 buffer refcount validator proof passed in
  `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_buffer_refcount_validator_20260628_081523`.
  The module `check` target rebuilt and installed the extension, ran the
  process-mode SQL regression, and reported TAP disabled for the configured
  tree. Direct `prove` then passed `011_phase16b_shell_pool_fallback.pl`,
  `012_phase16b_reusable_session_validator.pl`,
  `013_phase16b_shell_pool_state_isolation.pl`, and
  `014_phase16b_shell_pool_quarantine_destroy.pl` against the temp install.
  The validator now delegates private shared-buffer refcount scanning to the
  buffer manager, including resident array entries, hash entries, and buffer
  lock mode state, while preserving the existing idle-memory release predicate.
- Stage 5 pgstat/ipc validator proof passed in
  `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_pgstat_ipc_validator_20260628_082241`.
  `gmake -j18`, the backend-runtime module `check` target, direct `prove` for
  all four Phase 16B TAP files, and `PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3
  gmake check-runtime-lifecycles check-global-lifetimes` all passed. The
  validator now fails closed on pgstat backend-status/pending-state remnants and
  IPC/procsignal/shared-invalidation ownership markers, with DSM registry
  leftovers mapped to the existing `dsm_segments` reason.
- Stage 5 GUC baseline validator proof passed in
  `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_guc_baseline_validator_20260628_083257`.
  `gmake -j18`, the backend-runtime module `check` target, direct `prove` for
  all four Phase 16B TAP files, and `PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3
  gmake check-runtime-lifecycles check-global-lifetimes` all passed. The GUC
  subsystem now exposes a reset-baseline source/value predicate for per-session
  GUC contexts. The reusable-session validator uses it for the installed current
  session after the existing no-active-nest/stack/report checks, so session-level
  `SET` drift fails closed while `RESET` returns to the baseline.
- Stage 5 socket/FD validator proof passed in
  `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_socket_fd_validator_20260628_085124`.
  `gmake -j18`, the backend-runtime module `check` target, direct `prove` for
  all four Phase 16B TAP files, and `PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3
  gmake check-runtime-lifecycles check-global-lifetimes` all passed. The
  validator now fails closed on retained connection identity, cancel key, socket
  I/O buffer state, protocol wait-set state, client auth identity state, and
  security buffer state. File-access validation is delegated to `fd.c`, which
  can inspect live VFD entries while allowing an allocated but empty closed-state
  VFD cache.
- Stage 5 lock validator proof passed in
  `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_lock_validator_20260628_085954`.
  `gmake -j18`, the backend-runtime module `check` target, direct `prove` for
  all four Phase 16B TAP files, and `PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3
  gmake check-runtime-lifecycles check-global-lifetimes` all passed. The lock
  manager now owns reusable-lock-state validation: allocated empty local-lock
  scaffolding is allowed, while retained `LOCALLOCK` entries, nonzero fast-path
  counters, LWLocks, wait/deadlock state, predicate-lock state, and serializable
  lock state fail closed.
- Stage 5 procarray/procsignal validator proof passed in
  `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_procarray_procsignal_validator_20260628_090711`.
  `gmake -j18`, the backend-runtime module `check` target, direct `prove` for
  all four Phase 16B TAP files, and `PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3
  gmake check-runtime-lifecycles check-global-lifetimes` all passed. The
  validator now fails closed on pending backend interrupt/procsignal mailbox
  state, stored proc-die sender identity, retained local transaction ID,
  procarray cached completed-XID state, cached xmin horizon state, and global
  visibility horizon state. A new stable `procarray_state` reason distinguishes
  these procarray baseline failures from generic IPC ownership failures.
- Stage 5 real socket/FD no-leak proof passed in
  `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_socket_fd_real_no_leak_20260628_091526`.
  `gmake -j18`, the backend-runtime module `check` target, direct `prove` for
  all five Phase 16B TAP files, and `PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3
  gmake check-runtime-lifecycles check-global-lifetimes` all passed. The new
  runtime TAP exercises server-side `COPY` to/from a real file and `COPY TO
  STDOUT` over the real client socket, waits for shell validation after that
  workload exits, and verifies `action=destroy reusable=1 reason=ok` with no
  `storage_state` or `socket_attached` validation failure and no crash
  signatures.
- Stage 5 real buffer-pin reset-contract proof passed in
  `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_buffer_pin_real_reset_20260628_092319`.
  `gmake -j18`, the backend-runtime module `check` target, direct `prove` for
  all six Phase 16B TAP files, and `PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3
  gmake check-runtime-lifecycles check-global-lifetimes` all passed. The new
  runtime TAP exposes the live buffer-manager reusable-state predicate through
  the threaded test extension, then proves it returns clean after real heap
  insert, heap scan, and index scan work in the same shell session. The saved
  shell validation log records `action=destroy reusable=1 reason=ok` for that
  workload backend PID with no `buffer_pins` validation failure or crash
  signature.
- Stage 5 same-key GUC baseline proof passed in
  `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_same_key_guc_baseline_20260628_093248`.
  `gmake -j18`, the backend-runtime module `check` target, direct `prove` for
  all seven Phase 16B TAP files, and `PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3
  gmake check-runtime-lifecycles check-global-lifetimes` all passed. The new
  runtime TAP proves the reset-baseline predicate covers same-key role,
  database, role-in-database, and startup-packet GUC defaults. Each covered GUC
  fails the predicate after `SET`, returns to the configured baseline after
  `RESET`, and shell validation records `action=destroy reusable=1 reason=ok`
  with no `guc_state` validation failure. A different startup-packet option gets
  its own clean reset baseline.
- Stage 5 failed-validation no-leak proof passed in
  `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_failed_validation_no_leak_20260628_093924`.
  `gmake -j18`, the backend-runtime module `check` target, direct `prove` for
  all eight Phase 16B TAP files, and `PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3
  gmake check-runtime-lifecycles check-global-lifetimes` all passed. The new
  runtime TAP forces validation failure after a broad real workload that dirties
  temp table, prepared statement, LISTEN, session GUC, advisory lock,
  server-side file `COPY`, `COPY TO STDOUT` socket output, heap scan, and index
  scan paths. The shell log records `action=quarantine_destroy reusable=0` with
  nonzero quarantine/failure counters, and later clients observe no inherited
  temp/prepared/LISTEN/GUC/advisory-lock/buffer state while the persistent table
  remains readable.
- Post-Stage-5 churn checkpoint passed in
  `/home/sam/codex-work/mtpg-bench-results/phase16b_stage6_churn_full_20260628_094300`.
  The benchmark matrix runner now has a first-class `branch_shell` lane, mapping
  each `--pool-sizes` value to `threaded_session_pool=shell` and
  `threaded_session_pool_max=N`. Running the standard `connection_churn` shape
  for `branch_threaded` versus `branch_shell_64` (`duration=15`, `warmup=3`,
  `runs=3`, `clients=64`, `threads=16`) produced 1585.2 TPS for pinned
  `branch_threaded` and 2615.0 TPS for `branch_shell_64`, a 1.65x ratio with
  zero failed transactions.

Stage 0 baseline highlights
---------------------------

The Stage 0 run used the full Phase 16B baseline profile list:
`connection_churn`, `connection_churn_realish`, `pinned_hot`,
`pool_idle_100ms`, `pool_stateful_1000ms`, and `connection_memory_idle`.

| Profile | Key result | Gate read |
| --- | --- | --- |
| `connection_churn` | `branch_threaded` 1520.3 TPS, `branch_pool_64` 1942.8 TPS, `branch_process` 3311.2 TPS, `vanilla` 3456.3 TPS. | Existing pooling improves pure churn by 27.8% over pinned threaded, but still trails process mode and vanilla badly. |
| `connection_churn_realish` | `branch_threaded` 1199.0 TPS, best pooled lane `branch_pool_128` 1058.9 TPS, `branch_process` 2042.6 TPS. | Existing pooling fails the realish churn gate; it is 11.7% below pinned threaded. |
| `pinned_hot` | `branch_threaded` is 0.861x to 1.007x vanilla across the four hot workloads. | Hot execution is not the dominant regression; lifecycle remains the right focus. |
| `pool_idle_100ms` | `branch_threaded` 1985.2 TPS, best pooled lane 1977.8 TPS. | No throughput win from the existing pool on 100 ms idle. |
| `pool_stateful_1000ms` | `branch_threaded` 99.69 TPS, best pooled lane 99.02 TPS. | Stateful parked sessions do not show a throughput win. |
| `connection_memory_idle` | `branch_threaded` 994.3 TPS and 1028.7 KB PSS/client; best pooled throughput 953.1 TPS and about 613 KB PSS/client. | Pooling saves about 40% PSS/client at 1000 idle connections, but gives up about 4% throughput. |

Stage 0 decision: the baseline justifies measurement and upper-bound work. It
does not justify jumping directly to a warm backend pool.

Stage 1 lifecycle instrumentation
---------------------------------

The added instrumentation is disabled by default through
`log_threaded_lifecycle_timing=off`. The enabled run writes
`threaded_lifecycle_events.tsv` and `threaded_lifecycle_summary.tsv` in the
Stage 1 enabled directory.

Representative p50/p95 timings from the Stage 1 enabled run:

| Profile | Lane | Startup total us | Fork/shell us | InitPostgres us | Cleanup total us |
| --- | --- | ---: | ---: | ---: | ---: |
| `connection_churn` | `branch_process` | 2595 / 4720 | 399 / 1007 | 1605 / 3412 | 80 / 290 |
| `connection_churn` | `branch_threaded` | 9353 / 11968 | 7271 / 9757 | 1566 / 2940 | 295 / 554 |
| `connection_churn` | `branch_pool_64` | 6603 / 9016 | 4157 / 5817 | 1957 / 3625 | 632 / 1271 |
| `connection_churn_realish` | `branch_threaded` | 9391 / 13147 | 6654 / 10251 | 1884 / 4158 | 457 / 1004 |
| `connection_churn_realish` | `branch_pool_64` | 6927 / 10417 | 2113 / 5438 | 3874 / 5867 | 1820 / 2976 |
| `pool_stateful_1000ms` | `branch_threaded` | 9773 / 17854 | 6971 / 14726 | 1631 / 3108 | 16172 / 21786 |
| `pool_stateful_1000ms` | `branch_pool_64` | 9124 / 23109 | 4357 / 12307 | 3640 / 9586 | 23412 / 50115 |

Scheduler resume timing is not the primary churn bottleneck. For
`connection_churn`, pinned threaded scheduler attach p95 is 48 us and scheduler
total p50 is 41 us; the larger p95 totals are mostly wait time. For
`pool_idle_100ms` and `pool_stateful_1000ms`, scheduler total reflects the
intended client sleep interval, while attach p95 stays around 101 us and 95 us.

Stage 1 decision:

- Warm shell pooling is justified as the next proof target. Threaded startup is
  dominated by the shell/thread creation component, which is 6.7 ms to 7.3 ms
  at p50 on churn profiles.
- Async cleanup needed a retained-private-cleanup follow-up before
  implementation. The cleanup event above measures `PgBackendExitCleanup()`,
  while retained `TopMemoryContext` deletion happens later in the launch path.
- Warm backend pooling is not justified yet. `InitPostgres` is material, but
  the current pooled lanes regress realish churn and bursty wakeups, and no
  dirty-session validator exists yet.

Retained cleanup follow-up
--------------------------

After the Stage 1 cleanup timing split, an additional measurement-only event was
added for the retained private cleanup that runs after `PgBackendExitCleanup()`:
`threaded_lifecycle_retained_cleanup`. The event is still gated by
`log_threaded_lifecycle_timing=on` and is disabled by default.

| Artifact | Path |
| --- | --- |
| Result directory | `/home/sam/codex-work/mtpg-bench-results/phase16b_retained_cleanup_20260628_055213` |
| Branch install | `/home/sam/codex-work/mtpg-bench-results/phase16b_retained_cleanup_20260628_055213_branch_install` |
| Lifecycle events | `threaded_lifecycle_events.tsv` |
| Lifecycle summary | `threaded_lifecycle_summary.tsv` |

Validation:

- `gmake -j18` passed after adding the retained cleanup hook.
- `PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3 gmake check-runtime-lifecycles
  check-global-lifetimes` passed.
- The benchmark install was rebuilt clean with `--without-icu --disable-rpath
  --with-perl`; installed `pg_config --configure` points at the retained-cleanup
  branch install, and `USE_ASSERT_CHECKING` is undefined.
- The focused run covered `connection_churn`, `connection_churn_realish`, and
  `pool_stateful_1000ms` with zero failed transactions and no crash signatures.

Focused throughput with lifecycle timing enabled:

| Profile | Key rows |
| --- | --- |
| `connection_churn` | `branch_threaded` 1601.6 TPS; `branch_pool_64` 2081.9 TPS; `branch_pool_128` 2095.1 TPS; `branch_process` 3642.9 TPS; `vanilla` 3755.5 TPS. |
| `connection_churn_realish` | `branch_threaded` 1315.8 TPS; best pooled lane `branch_pool_64` 1150.8 TPS; `branch_process` 2320.6 TPS; `vanilla` 2380.1 TPS. |
| `pool_stateful_1000ms` | `branch_threaded` 98.68 TPS; best pooled lane `branch_pool_64` 94.91 TPS; `branch_process` 98.62 TPS; `vanilla` 98.79 TPS. |

Retained private cleanup p50/p95 timings:

| Profile | Lane | Samples | Total us | Delete us p95 | Freelist us p95 | Accounted / reclaimed bytes p50 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `connection_churn` | `branch_threaded` | 9605 | 11 / 121 | 31 | 99 | 523840 / 67584 |
| `connection_churn` | `branch_pool_64` | 12426 | 208 / 718 | 132 | 627 | 515648 / 59392 |
| `connection_churn_realish` | `branch_threaded` | 7907 | 48 / 307 | 49 | 256 | 766880 / 67584 |
| `connection_churn_realish` | `branch_pool_64` | 6904 | 1086 / 2126 | 224 | 1987 | 758688 / 59392 |
| `pool_stateful_1000ms` | `branch_threaded` | 204 | 72 / 858 | 148 | 644 | 1229776 / 228400 |
| `pool_stateful_1000ms` | `branch_pool_32` | 204 | 1140 / 10887 | 898 | 10635 | 1251104 / 220208 |
| `pool_stateful_1000ms` | `branch_pool_64` | 204 | 2602 / 7326 | 924 | 6149 | 1251104 / 220208 |

Retained cleanup decision:

- Dedicated threaded retained cleanup is not a Stage 16B throughput bottleneck:
  churn p95 is 121 us, realish churn p95 is 307 us, and the stateful p95 is
  858 us.
- Current pooled stateful exits do show a retained-cleanup tail, mostly in
  freelist draining, but those pooled lanes still lose throughput to pinned
  threaded and process mode.
- `PgBackendExitCleanup()` still has the larger stateful tail: for
  `pool_stateful_1000ms`, cleanup p95 is 17.8 ms on pinned threaded and 47.8 ms
  to 53.6 ms on pooled lanes, mostly in `shmem_exit`.
- Async private cleanup is therefore not justified as the next implementation
  step. Revisit it after a warm shell proof or if a stateful disconnect-tail
  target becomes a product requirement.

Stage 2 upper-bound proof
-------------------------

The Stage 2 run added the missing short-idle burst profile:
`pool_burst_10ms` with lifecycle timing enabled.

| Lane | TPS | Latency ms | Ratio vs pinned threaded |
| --- | ---: | ---: | ---: |
| `branch_threaded` | 19398.5 | 10.310 | 1.000 |
| `branch_pool_64` | 6029.7 | 33.169 | 0.311 |
| `branch_pool_128` | 5373.1 | 37.223 | 0.277 |
| `branch_pool_192` | 5480.9 | 36.490 | 0.283 |

Pinned threaded scheduler overhead on this burst profile remains small:
`threaded_lifecycle_scheduler` p50/p95 total is 86/206 us, wait is 60/177 us,
and attach is 20/40 us.

Stage 2 decision:

- The existing pooled carrier path fails the burst proof; it should not be used
  as evidence for a full warm backend pool.
- A warm shell proof is still justified by the startup upper bound and the
  pure-churn pool64 improvement, because it can remove shell creation without
  reusing dirty SQL session state.
- A full warm backend pool remains deferred until after shell/cleanup proof
  runs and explicit fail-closed dirty-state validation are in place.

Stage 4 shell carrier proof
---------------------------

The Stage 4 proof added a disabled-by-default `threaded_session_pool=shell`
mode with `threaded_session_pool_max`. This is not SQL-session reuse and does
not hand a retired authenticated backend to a new client. It reuses idle shell
carrier threads only when one is already available, runs normal startup and
authentication for each client, and still destroys the logical backend at
disconnect. If no idle shell carrier is available, the current client uses the
normal dedicated threaded path and the shell pool may warm another idle carrier
for later reuse. Thus `threaded_session_pool_max` is a reusable-carrier cache
limit, not a connection admission limit. The existing pooled protocol scheduler
remains separate and is only enabled by `pooled_protocol_carriers`.

Artifact directories:

| Artifact | Path |
| --- | --- |
| Build/install base | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_proof_20260628_061003` |
| Branch install | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_proof_20260628_061003_branch_install` |
| Lifecycle-on smoke, shell off | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_proof_20260628_061003/smoke_off` |
| Lifecycle-on smoke, shell enabled | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_proof_20260628_061003/smoke_shell` |
| Focused churn, shell off | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_proof_20260628_061003/proof_off_churn_c64` |
| Focused churn, shell enabled | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_proof_20260628_061003/proof_shell_churn_c64` |
| Pinned hot guard, shell off | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_proof_20260628_061003/proof_off_pinned_c64` |
| Pinned hot guard, shell enabled | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_proof_20260628_061003/proof_shell_pinned_c64` |
| Process-mode smoke | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_proof_20260628_061003/process_smoke` |
| Final lifecycle-on shell counter smoke | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_counters_20260628_062840/counter_smoke_idleonly_shell` |
| Final shell churn proof | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_counters_20260628_062840/proof_shell_idleonly_churn_c64` |
| Final checkpoint, shell off | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_counters_20260628_062840/checkpoint_off` |
| Final checkpoint, pinned hot shell | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_counters_20260628_062840/checkpoint_shell_idleonly_pinned_hot` |
| Final checkpoint, idle 100 ms shell | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_counters_20260628_062840/checkpoint_shell_idleonly_pool_idle_100ms` |
| Final checkpoint, burst 10 ms shell | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_counters_20260628_062840/checkpoint_shell_idleonly_pool_burst_10ms` |
| Final checkpoint, stateful 1000 ms shell | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_counters_20260628_062840/checkpoint_shell_idleonly_pool_stateful_1000ms` |
| Final checkpoint, c1000 memory shell | `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_counters_20260628_062840/checkpoint_shell_idleonly_connection_memory_idle` |

Validation:

- `gmake -j18` passed after the shell proof implementation.
- `PHASE16_LOCAL_DEBROOT=/ MALLOC_CHECK_=3 gmake check-runtime-lifecycles
  check-global-lifetimes` passed after the shell proof implementation.
- The proof install was rebuilt clean with `--without-icu --disable-rpath
  --with-perl`; installed headers show `USE_ASSERT_CHECKING` undefined.
- All completed Stage 4 benchmark rows below reported `failed_transactions = 0`;
  targeted log sweeps found no `FATAL`, `PANIC`, segmentation, server-closed,
  or nonzero-failure signatures.
- A process-mode smoke run completed with `branch_process`
  `select1_connect_prepared` at 2223.3 TPS and zero failed transactions.
- Direct TAP proof
  `prove -I src/test/perl src/test/modules/test_backend_runtime/t/011_phase16b_shell_pool_fallback.pl`
  passed with the Stage 4 branch install on `PATH`. The test configures
  `threaded_session_pool=shell`, `threaded_session_pool_max=2`, admits five
  concurrent clients, and verifies dedicated fallback accounting without
  carrier start failures.
- An intermediate bounded-admission shell attempt intentionally remains as a
  negative artifact: with `clients=200` and `threaded_session_pool_max=64`,
  `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_counters_20260628_062840/checkpoint_shell/pool_idle_100ms`
  stalled during pgbench connection startup. A first fallback attempt,
  `/home/sam/codex-work/mtpg-bench-results/phase16b_shell_counters_20260628_062840/checkpoint_shell_fallback_pool_idle_100ms`,
  reached 195 idle sessions but left 5 clients unstarted. The final idle-only
  carrier cache removes this admission behavior.

Lifecycle-on smoke used `clients=16`, `threads=8`, `duration=3`,
`warmup=1`, `runs=1`, `max_connections=64`, and enabled
`log_threaded_lifecycle_timing=on`:

| Mode | Workload | TPS | Latency ms | Failed txns |
| --- | --- | ---: | ---: | ---: |
| `off` | `select1_connect_prepared` | 1630.8 | 9.811 | 0 |
| `shell max=16` | `select1_connect_prepared` | 2919.3 | 5.481 | 0 |
| `shell max=16, idle-only` | `select1_connect_prepared` | 2564.7 | 6.239 | 0 |

The final counter smoke emitted parseable `threaded_session_pool_stats` rows
only when lifecycle timing was enabled. The last row recorded:
`requested_starts=10245`, `successful_starts=10245`, `start_failures=0`,
`carrier_starts=16`, `idle_carrier_waits=9379`,
`carrier_limit_fallbacks=1311`, and `destroy_paths=8934`.

The focused throughput proof used `clients=64`, `threads=16`, `duration=8`,
`warmup=2`, `runs=2`, `max_connections=128`, and
`threaded_session_pool_max=64`. Lifecycle timing was left disabled for these
throughput rows.

| Profile | Mode | TPS | Latency ms | Failed txns | Ratio vs off |
| --- | --- | ---: | ---: | ---: | ---: |
| `connection_churn` | `off` | 1594.9 | 40.128 | 0 | 1.000 |
| `connection_churn` | `shell max=64` | 2631.2 | 24.324 | 0 | 1.650 |
| `connection_churn` | `shell max=64, idle-only` | 2653.0 | 24.124 | 0 | 1.663 |
| `connection_churn_realish` | `off` | 1326.6 | 48.249 | 0 | 1.000 |
| `connection_churn_realish` | `shell max=64` | 1909.8 | 33.513 | 0 | 1.440 |
| `connection_churn_realish` | `shell max=64, idle-only` | 1883.9 | 33.973 | 0 | 1.420 |

Per-run samples were tight for the churn proof: pure churn shell rows were
2627.2 and 2635.1 TPS, while off rows were 1596.9 and 1592.9 TPS. Realish
churn shell rows were 1921.6 and 1898.0 TPS, while off rows were 1314.6 and
1338.6 TPS.

The pinned-session guard used the same `clients=64`, `threads=16`,
`duration=8`, `warmup=2`, `runs=2`, and `max_connections=128` shape on
`builtin_select_prepared`:

| Mode | TPS | Latency ms | Failed txns | Ratio vs off |
| --- | ---: | ---: | ---: | ---: |
| `off` | 426389.7 | 0.151 | 0 | 1.000 |
| `shell max=64` | 409745.6 | 0.157 | 0 | 0.961 |

The final broader checkpoint used short one-run profile slices with shell off
versus `threaded_session_pool=shell`, `threaded_session_pool_max=64`, and
lifecycle timing disabled:

| Profile | Workload | Off TPS | Shell TPS | Failed txns | Shell ratio |
| --- | --- | ---: | ---: | ---: | ---: |
| `pinned_hot` | `builtin_select_prepared` | 242806.1 | 241245.7 | 0 | 0.994 |
| `pinned_hot` | `select1_prepared` | 400070.1 | 392707.1 | 0 | 0.982 |
| `pinned_hot` | `bench_one_prepared` | 276050.9 | 276912.0 | 0 | 1.003 |
| `pinned_hot` | `kv_read_prepared` | 238441.3 | 236114.1 | 0 | 0.990 |
| `pool_idle_100ms` | `select1_sleep_wake_100ms_prepared` | 1981.2 | 1981.5 | 0 | 1.000 |
| `pool_burst_10ms` | `select1_sleep_wake_10ms_prepared` | 19441.8 | 19424.0 | 0 | 0.999 |
| `pool_stateful_1000ms` | `stateful_temp_sleep_wake_1000ms_prepared` | 98.50 | 98.66 | 0 | 1.002 |
| `connection_memory_idle` | `select1_sleep_wake_1000ms_prepared` | 979.2 | 984.4 | 0 | 1.005 |

Resource rows confirm that idle-only shell pooling does not reduce thread or
memory footprint for persistent-client profiles. At c200 100 ms idle, both off
and shell used 208 server threads; shell PSS was 244903 KB versus 238670 KB off.
At c1000 idle, both used 1008 server threads; shell PSS was 1129147 KB versus
1113551 KB off.

Stage 4 decision:

- The shell carrier proof clears the Stage 2/4 churn throughput gate: the final
  idle-only shell cache improved pure churn by 66.3% and realish churn by 42.0%,
  both well above the 10% gate.
- The steady-state checkpoint does not justify using shell pooling as a general
  persistent-session pool or enabling it by default. It preserves TPS once
  dedicated fallback is used, but it does not reduce threads or memory for
  persistent clients.
- Async private cleanup remains deferred. The strongest measured win came from
  avoiding foreground thread/shell creation, not from retained private cleanup.
- Full warm backend pooling remains deferred. The current shell proof has no
  reusable SQL session. Stage 5 now has an initial dirty-state validator and
  destroy-path validation logging plus same-key role/database/startup GUC
  baseline proof, but it still lacks broader real failed-validation coverage.
  That must be added before any authenticated backend/session reuse is
  considered.

Stage 5 validator scaffold
--------------------------

The first Stage 5 slice added a central reusable-session validator with stable
reason codes and names. The current checks cover null runtime objects, optional
transaction activity, attached `PGPROC`/proc-number state, attached client
socket state, retained connection identity/cancel-key/socket I/O/protocol/client
auth/security state, prepared statements, portals, LISTEN state, temp
namespaces, extension private state, DSM segments, execution resource owners,
execution memory context roots, active timeouts, lock state, buffer pin
indicators, temp file state, GUC stack/list state, GUC reset-baseline
source/value drift for per-session GUC contexts, plan-cache lists,
snapshots/combo CIDs, invalidation state, storage FD/live-VFD/sync state, XLog
insert-in-progress state, pending async actions, resident private shared-buffer
refcount entries, allocated-empty versus retained local-lock hash state,
fast-path lock counters, pgstat backend-status/pending-state remnants, pending
backend interrupt/procsignal mailbox state, retained procarray local transaction
ID/cached-XID/horizon state, and IPC/procsignal/shared-invalidation ownership
markers.

The shell carrier destroy path now runs the validator before destroying the
logical backend. With `log_threaded_lifecycle_timing=on`, shell-pool logs
include `validation_passes` and `validation_failures` counters plus a
`threaded_session_pool_validation` reason row with the per-reason cumulative
`reason_count`. Failed validation is counted separately as
`quarantine_destroy_paths`, and validation rows use
`action=quarantine_destroy` for the fail-closed path. The path still destroys
the logical backend for every disconnect; this is measurement and fail-closed
observability, not SQL-session reuse.

Artifacts:

| Result | Directory or log |
| --- | --- |
| Stage 5 validator proof base | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_validator_20260628_074210` |
| Module build/check log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_validator_20260628_074210/module_check.log` |
| Direct TAP log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_validator_20260628_074210/direct_prove.log` |
| Expanded validator proof base | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_validator_expanded_20260628_074953` |
| Expanded module build/check log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_validator_expanded_20260628_074953/module_check.log` |
| Expanded direct TAP log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_validator_expanded_20260628_074953/direct_prove.log` |
| Shell state-isolation proof base | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_shell_state_isolation_20260628_075754` |
| Shell state-isolation module check log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_shell_state_isolation_20260628_075754/module_check.log` |
| Shell state-isolation direct TAP log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_shell_state_isolation_20260628_075754/direct_prove.log` |
| Quarantine destroy proof base | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_quarantine_destroy_20260628_080648` |
| Quarantine destroy module check log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_quarantine_destroy_20260628_080648/module_check.log` |
| Quarantine destroy direct TAP log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_quarantine_destroy_20260628_080648/direct_prove.log` |
| Buffer refcount validator proof base | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_buffer_refcount_validator_20260628_081523` |
| Buffer refcount module check log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_buffer_refcount_validator_20260628_081523/module_check.log` |
| Buffer refcount direct TAP log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_buffer_refcount_validator_20260628_081523/direct_prove.log` |
| Pgstat/ipc validator proof base | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_pgstat_ipc_validator_20260628_082241` |
| Pgstat/ipc build log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_pgstat_ipc_validator_20260628_082241/build.log` |
| Pgstat/ipc module check log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_pgstat_ipc_validator_20260628_082241/module_check.log` |
| Pgstat/ipc direct TAP log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_pgstat_ipc_validator_20260628_082241/direct_prove.log` |
| Pgstat/ipc lifecycle/global log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_pgstat_ipc_validator_20260628_082241/lifecycle_global_checks.log` |
| GUC baseline validator proof base | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_guc_baseline_validator_20260628_083257` |
| GUC baseline build log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_guc_baseline_validator_20260628_083257/build.log` |
| GUC baseline module check log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_guc_baseline_validator_20260628_083257/module_check.log` |
| GUC baseline direct TAP log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_guc_baseline_validator_20260628_083257/direct_prove.log` |
| GUC baseline lifecycle/global log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_guc_baseline_validator_20260628_083257/lifecycle_global_checks.log` |
| Socket/FD validator proof base | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_socket_fd_validator_20260628_085124` |
| Socket/FD build log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_socket_fd_validator_20260628_085124/build.log` |
| Socket/FD module check log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_socket_fd_validator_20260628_085124/module_check.log` |
| Socket/FD direct TAP log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_socket_fd_validator_20260628_085124/direct_prove.log` |
| Socket/FD lifecycle/global log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_socket_fd_validator_20260628_085124/lifecycle_global_checks.log` |
| Lock validator proof base | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_lock_validator_20260628_085954` |
| Lock validator build log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_lock_validator_20260628_085954/build.log` |
| Lock validator module check log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_lock_validator_20260628_085954/module_check.log` |
| Lock validator direct TAP log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_lock_validator_20260628_085954/direct_prove.log` |
| Lock validator lifecycle/global log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_lock_validator_20260628_085954/lifecycle_global_checks.log` |
| Procarray/procsignal validator proof base | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_procarray_procsignal_validator_20260628_090711` |
| Procarray/procsignal build log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_procarray_procsignal_validator_20260628_090711/build.log` |
| Procarray/procsignal module check log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_procarray_procsignal_validator_20260628_090711/module_check.log` |
| Procarray/procsignal direct TAP log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_procarray_procsignal_validator_20260628_090711/direct_prove.log` |
| Procarray/procsignal lifecycle/global log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_procarray_procsignal_validator_20260628_090711/lifecycle_global_checks.log` |
| Real socket/FD no-leak proof base | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_socket_fd_real_no_leak_20260628_091526` |
| Real socket/FD build log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_socket_fd_real_no_leak_20260628_091526/build.log` |
| Real socket/FD module check log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_socket_fd_real_no_leak_20260628_091526/module_check.log` |
| Real socket/FD direct TAP log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_socket_fd_real_no_leak_20260628_091526/direct_tap.log` |
| Real socket/FD lifecycle/global log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_socket_fd_real_no_leak_20260628_091526/lifecycle_global.log` |
| Real socket/FD cluster logs | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_socket_fd_real_no_leak_20260628_091526/direct_tap_cluster_log` |
| Real buffer-pin reset proof base | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_buffer_pin_real_reset_20260628_092319` |
| Real buffer-pin reset build log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_buffer_pin_real_reset_20260628_092319/build.log` |
| Real buffer-pin reset module check log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_buffer_pin_real_reset_20260628_092319/module_check.log` |
| Real buffer-pin reset direct TAP log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_buffer_pin_real_reset_20260628_092319/direct_tap.log` |
| Real buffer-pin reset lifecycle/global log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_buffer_pin_real_reset_20260628_092319/lifecycle_global.log` |
| Real buffer-pin reset cluster logs | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_buffer_pin_real_reset_20260628_092319/direct_tap_cluster_log` |
| Same-key GUC baseline proof base | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_same_key_guc_baseline_20260628_093248` |
| Same-key GUC baseline build log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_same_key_guc_baseline_20260628_093248/build.log` |
| Same-key GUC baseline module check log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_same_key_guc_baseline_20260628_093248/module_check.log` |
| Same-key GUC baseline direct TAP log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_same_key_guc_baseline_20260628_093248/direct_tap.log` |
| Same-key GUC baseline lifecycle/global log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_same_key_guc_baseline_20260628_093248/lifecycle_global.log` |
| Same-key GUC baseline cluster logs | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_same_key_guc_baseline_20260628_093248/direct_tap_cluster_log` |
| Failed-validation no-leak proof base | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_failed_validation_no_leak_20260628_093924` |
| Failed-validation no-leak build log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_failed_validation_no_leak_20260628_093924/build.log` |
| Failed-validation no-leak module check log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_failed_validation_no_leak_20260628_093924/module_check.log` |
| Failed-validation no-leak direct TAP log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_failed_validation_no_leak_20260628_093924/direct_tap.log` |
| Failed-validation no-leak lifecycle/global log | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_failed_validation_no_leak_20260628_093924/lifecycle_global.log` |
| Failed-validation no-leak cluster logs | `/home/sam/codex-work/mtpg-bench-results/phase16b_stage5_failed_validation_no_leak_20260628_093924/direct_tap_cluster_log` |

Decision gate:

- The initial Stage 5 dirty-state reason-code scaffold clears the narrow
  validator-skeleton gate, now with broader retained-state coverage:
  deliberately injected dirty states return expected reasons, and shell
  fallback behavior still passes.
- The shell destroy-path state-isolation gate clears for the covered runtime
  states: temp table namespace, prepared statement, LISTEN, session GUC drift,
  and a session advisory lock did not leak to subsequent clients, and shell
  validation logs include per-reason accounting.
- The quarantine destroy accounting gate clears for the covered forced-failure
  path: failed shell validation logs `action=quarantine_destroy`, increments
  `quarantine_destroy_paths`, records a non-valid dirty reason, and still
  destroys the logical backend without leaking dirty state.
- The buffer refcount validator gate clears for the covered synthetic dirty
  states: resident private refcount array/hash entries and private buffer lock
  modes fail closed with `buffer_pins` in both process-mode and threaded
  validator tests.
- The real buffer-pin reset-contract gate clears for the covered runtime path:
  after real heap insert, heap scan, and index scan activity, the live
  buffer-manager reusable-state predicate returns clean in the same shell
  session, and shell destroy validation later records
  `action=destroy reusable=1 reason=ok` for that backend PID with no
  `buffer_pins` failure.
- The pgstat/ipc validator gate clears for the covered synthetic dirty states:
  pgstat backend-status pointers, pgstat pending-local state, procsignal slots,
  shared-invalidation buffers, and DSM registry leftovers fail closed with
  stable reason codes in both process-mode and threaded validator tests.
- The GUC baseline validator gate clears for the covered per-session GUC
  contexts: a session `SET` changes source/value state away from the reset
  baseline and returns to clean after `RESET` in both process-mode regression and
  threaded TAP proof. Fixed internal/server GUCs such as `data_checksums` are
  intentionally outside the session baseline comparison.
- The same-key role/database/startup GUC gate clears for the covered runtime
  path: database defaults, role defaults, role-in-database defaults, and
  startup-packet options all participate in the reset baseline. The saved TAP
  proof observes dirty state after `SET`, clean state after `RESET`, clean state
  for the next same-key client, a distinct clean baseline for a different
  startup option, and shell validation rows with `reason=ok` and no
  `guc_state` failure.
- The socket/FD validator gate clears for the covered synthetic dirty states:
  retained connection identity/cancel-key, socket I/O buffers, protocol wait
  sets, client auth identity, GSS/security buffers, external FDs, open virtual
  files, allocated descriptors, pending sync ops, and unpinned smgr lists fail
  closed with stable reason codes in both process-mode and threaded validator
  tests.
- The real socket/FD no-leak gate clears for the covered runtime path:
  server-side `COPY` to/from a real file and `COPY TO STDOUT` through the real
  client socket leave the shell carrier reusable. The saved cluster log records
  `action=destroy reusable=1 reason=ok` after the workload session exits and
  contains no `storage_state`, `socket_attached`, or crash signatures.
- The lock validator gate clears for the covered synthetic dirty states:
  allocated-empty local-lock scaffolding is accepted as reusable, while retained
  local-lock hash entries, fast-path lock counters, LWLocks, wait/deadlock
  state, predicate-lock state, and serializable lock state fail closed through
  the lock-manager-owned predicate.
- The procarray/procsignal validator gate clears for the covered synthetic dirty
  states: pending backend interrupt/procsignal mailbox state, stored proc-die
  sender identity, retained local transaction ID, procarray cached completed-XID
  state, cached xmin horizon state, and global visibility horizon state fail
  closed with stable reason codes in both process-mode and threaded validator
  tests.
- The broad failed-validation no-leak gate clears for the covered runtime path:
  forced validation failure after temp table, prepared statement, LISTEN,
  session GUC, advisory lock, server-side file `COPY`, `COPY TO STDOUT`, heap
  scan, and index scan activity records `action=quarantine_destroy reusable=0`
  and later clients observe no inherited temp/prepared/LISTEN/GUC/advisory-lock
  or buffer state.
- Full Stage 5 clears for the covered Phase 16B contract: validation has stable
  reason codes and counters, clean shell destroy paths validate as reusable,
  dirty/forced failed paths destroy/quarantine fail closed, and the saved TAP
  evidence covers reset contracts plus later-client no-leak checks.
- The post-Stage-5 churn checkpoint clears the Phase 16B pure connection-churn
  performance target for the covered benchmark shape: `branch_shell_64` is
  1.65x pinned `branch_threaded` with zero failed transactions.
- Stage 6 warm backend pooling is unblocked by the validator work but remains
  deferred for the pure churn target because warm shell pooling already exceeds
  pinned-thread churn. A conservative same-database/same-role backend prototype
  should be treated as follow-on work for authenticated SQL-session reuse or
  realish-churn targets, not as a requirement to close this Phase 16B gate.
