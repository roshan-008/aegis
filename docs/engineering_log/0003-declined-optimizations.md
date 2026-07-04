# 0003 — Declined optimizations: matmul intrinsics; client-side Nagle

- **Status:** declined (two decisions, both "no change after investigation")
- **Subsystems:** `src/kernels/core.hpp` (matmul); `src/net/feed.hpp`, `rust/src/lib.rs`
- **CS concept:** compiler codegen / instruction selection; TCP (Nagle, delayed-ACK)
- **Host of record:** Apple M1, clang 21 `-O2 -march=native`; disassembly + code read

Deciding *not* to change something after investigating it is an engineering
decision. These two are logged because the investigation, not the diff, is the
value — and because "I checked and left it alone" is a stronger interview answer
than a change made on reflex.

---

## A. Hand-written matmul intrinsics — declined

### Problem / temptation
`best::matmul` is cache-blocked (BS=32) with an `std::fma` inner loop and runs at
~8–9 GF/s (3.6–4.7× over naive, `bench_kernels`). The obvious "next step" a
portfolio instinct suggests: hand-write NEON/AVX intrinsics for the microkernel.

### Measurement (the deciding evidence)
Disassembled the kernels at `-O2 -march=native` instead of guessing:
- `best::matmul` / `best::dot`: **14 `fmadd`/`fmla`** instructions emitted.
- reductions: **18 NEON `.2d`** vector ops with paired `ldp q0,q1` 128-bit loads.

The compiler is *already* emitting fused-multiply-add and vector loads. Command:
`clang++ -O2 -march=native -S` then grep `fmadd|fmla|\.2d`. Real excerpt from the
`best::dot` inner loop:

```asm
    ldp   q0, q1, [x0]          ; paired 128-bit loads
    ...
    fmadd d1, d4, d5, d1        ; d1 = d4*d5 + d1   (accumulator 1)
    fmadd d0, d6, d7, d0        ; d0 = d6*d7 + d0   (accumulator 0)
```

Two independent accumulators (`d0`, `d1`) fed by fused multiply-adds — the
compiler applied exactly the multiply-accumulate + dependency-chain-breaking
pattern (H&P, ILP) that a hand-written intrinsic loop would aim for.

### References consulted
- *Effective Modern C++* + the Compiler Explorer habit: verify codegen before
  hand-optimizing.
- Hennessy & Patterson: the win here is *blocking* (cache reuse), which the
  speedup-grows-with-size curve confirms — not instruction selection.

### Decision
**Declined.** Hand intrinsics would add ISA-specific code (a NEON path and an AVX
path, each to test and maintain) to reproduce instructions the compiler already
generates. The measured win comes from the memory-access pattern, which is
already in place. Intrinsics would be maintenance cost for ~0 gain.

### What would change the decision
A profile showing the microkernel is FLOP-bound and *not* hitting peak FMA
throughput — i.e., evidence the compiler's schedule is the bottleneck. No such
evidence exists here; the roofline says these sizes are cache/bandwidth-shaped.

---

## B. Client-side `TCP_NODELAY` — declined (already correct where it matters)

### Problem / temptation
Stevens (*UNIX Network Programming*) makes disabling Nagle the reflex fix for
hand-rolled TCP latency. Easy to "add `TCP_NODELAY` everywhere" without checking.

### Measurement (the deciding evidence)
Read both ends of the feed:
- Rust server (`serve_store`): already calls `set_nodelay(true)` on the accepted
  stream — Nagle is off on the **write side**, which is the side that sends ticks.
- C++ client (`feed.hpp`): **receive-only** (`readv` in a loop, never `send`).
  Nagle only affects data a socket *sends*, so client-side Nagle is moot.

### References consulted
- Stevens, UNP — Nagle's algorithm and the Nagle/delayed-ACK interaction.

### Decision
**Declined.** The write side that matters already disables Nagle; the client
sends nothing. Adding `TCP_NODELAY` to the client would be a no-op cargo-culted
from the reference. Also declined: swapping blocking sockets for `epoll`/`kqueue`
— replay is a single connection, so an event loop is machinery without benefit at
this scale (it becomes correct only for many concurrent feeds).

### What would change the decision
A bidirectional protocol (client sending requests/acks), or many concurrent
feeds — either would revive both the client-Nagle and the event-loop questions.

---

## Surprise
I expected a competitive matmul to *need* hand intrinsics — that's the folklore.
The surprise was opening the assembly and finding FMA and paired vector loads
already there; the entire measured win was cache blocking, an access-pattern
change, not instruction selection. The networking surprise was smaller but real:
the reflex "add TCP_NODELAY" had nowhere to go, because the one socket that sends
already had it and the other never sends.

## Looking back
Both declines hold. The matmul one is contingent on a fact I should keep
re-checking: *if* a future kernel became FMA-port-bound, the compiler's schedule
could become the ceiling and intrinsics (or at least `#pragma`-guided unrolling)
would re-enter. The right habit isn't "never write intrinsics," it's "read the
asm and the roofline first" — which is what happened here.

## Open questions
- `best::matmul` plateaus at ~8–9 GF/s, well under the M1's FP peak. Is it
  bandwidth-bound, or missing register-tiling (accumulating a small output tile in
  registers across the k-loop)? **I can't tell without a roofline + `perf`
  counters (Linux).** This is the most interesting open question in the repo.
- Does the arm64 vs x86/AVX2 codegen differ enough that the "no intrinsics"
  decision flips on one target? Needs the Linux/x86 build to compare.
