# 0006 — Work-stealing scheduler: removing the level barrier (2.4× on skew)

- **Status:** shipped (`src/runtime/work_steal.hpp`), level scheduler kept as the simple default
- **Subsystem:** `src/runtime/` — DAG execution
- **CS concept:** work stealing (Cilk/TBB), barrier vs dataflow scheduling, lost-wakeup races
- **Host of record:** Apple M1, `bench_scheduler`, 8 workers, 8 chains × 64 nodes

## Problem
`Scheduler::submit` executes a graph level by level with a full join between
levels: every task in level *k* must finish before anything in level *k+1*
starts. A level's wall time is therefore its **slowest** task, and the run's
wall time is the **sum of per-level maxima**. With skewed task durations the
pool idles behind one straggler per level. The trace made this concrete before
any fix: 22% pool efficiency (busy ÷ span × workers) on a skewed graph.

## Hypothesis
Dependency-counting dispatch (a node runs the moment its own inputs finish)
drops the bound from Σ per-level maxima to the **critical path** — for
independent chains, the heaviest chain sum. An analytical model from the
generated durations predicted 16.5 ms (barrier) vs 7.0 ms (critical path).

## Alternatives considered
- **Lock-free Chase-Lev deques** — the textbook implementation. Declined:
  per-task cost here is dominated by kernel work (µs–ms), not deque ops, and
  the project's one-concurrency-primitive discipline (`ring_buffer.hpp`) is
  worth keeping. Mutexed deques measure fine at this granularity.
- **Keep the barrier, sort levels by duration** — helps only when durations
  are known up front; they aren't.

## Mechanics
Per-worker deque: owner pushes/pops the **back** (LIFO keeps its own freshly
written data cache-warm), thieves take the **front** (FIFO: oldest, largest
remaining subtree — fewest steals). A finishing node decrements each
dependent's pending count and pushes newly ready ones onto **its own** deque
(the dependent reads what this worker just wrote). Idle workers scan all
deques, then sleep on an epoch counter bumped by every push. First exception
cancels the run, drains remaining nodes without invoking, and rethrows from
`submit` — the scheduler stays reusable.

## The bug the bring-up found (and the fix)
First version captured the wake epoch **after** the failed deque scan. A push
landing between the scan and the capture was invisible: the worker then slept
on an epoch that already included the push. With all workers doing this, the
last wakeup was lost — observed as a hard hang in `test_runtime`'s randomized
DAG round. Fix: capture the epoch **before** scanning; any push after the
capture changes the epoch and the wait predicate refuses to sleep. This is
the classic lost-wakeup TOCTOU, and it is why the epoch read is the first
statement of the idle iteration, with a comment saying exactly that.

## Measurement (`bench_scheduler`, oracle-checked)
Both schedulers run the identical pre-generated graph; output checksums must
match or the run doesn't count.

| durations | model: barrier / critical path | level | stealing | speedup |
|---|---|---:|---:|---:|
| uniform 50 µs (control) | 3.2 / 3.2 ms | 8.3 ms | 4.8 ms | ~1.2–1.7× |
| skewed (1-in-8 nodes 20×) | 16.5 / 7.0 ms | 18.9 ms | 7.7 ms | **2.4–3.6×** |

Traced skewed run: efficiency 22.1% → **74.0%**; measured critical path
tracks the model. The uniform control also exposes the level scheduler's
fixed overhead (packaged_task + future + barrier per level ≈ 80 µs/level):
its measured 8.3 ms vs the 3.2 ms model is overhead, not skew.

## Trade-offs
- Mutex per deque: fine at kernel-task granularity; wrong below ~1 µs tasks.
- One graph in flight (`submit` serialized) — same contract as `Scheduler`.
- The uniform speedup is noise-sensitive (0.93–1.7× observed under desktop
  load), so the regression gate tracks only the skewed case, against a
  conservative 2.4× floor.

## Future
Linux CI host: pinned-thread runs, per-steal cache-miss counters via `perf`.
