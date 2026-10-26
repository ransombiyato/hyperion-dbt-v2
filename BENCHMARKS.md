# BENCHMARKS

> **Scope of these numbers (2026-10-23).** Everything below measures the two
> execution tiers on **single-threaded, non-forking** workloads: the five
> `benchmarks/workloads/*.c` programs are straight-line compute, and
> `hyperion_tier_bench` is a synthetic loop. None of them create a subprocess.
> The process-creation syscalls added since (`fork`/`vfork`/`clone` subset/
> `execve`/`wait4`) are therefore **not represented in any figure here**, and a
> real shell pipeline costs a host `fork` plus a deep copy of the guest address
> space per child — a cost these numbers do not include and do not claim to.
> `hyperion_bench` measures the *host* machine, not emulation.

Reproducible methodology and measured results.

> **Honesty rule (D-009):** a benchmark report may never contain a fabricated
> number. Metrics that cannot yet be measured are reported as `unavailable`
> with a reason. Every result is attributable to a host, compiler, build
> configuration and methodology.

---

## 1. Why a benchmark harness exists in Phase 0

Measurement infrastructure must exist and be trusted *before* there is anything
to optimize. The Phase 0 harness establishes the reporting format, records the
environment fingerprint, and guards against broken builds. It measures **host**
and **harness** characteristics only.

---

## 2. Environment fingerprint

Every run records and reports:

| Field | Source | Why it matters |
| --- | --- | --- |
| architecture | `host_probe` | x86-64 vs AArch64 changes everything |
| kernel release | `uname` | affects syscall/feature availability |
| logical CPUs | `hardware_concurrency` | affects threading benchmarks |
| page size | `sysconf(_SC_PAGESIZE)` | affects code-cache and mmap design |
| cache-line size | `sysconf(_SC_LEVEL1_DCACHE_LINESIZE)` | affects code alignment |
| build identity | `build_summary()` | compiler + build type affect codegen |
| commit SHA | recorded by the runner | ties results to source |

The machine-readable form is the `host` and `build` objects in the JSON output.

---

## 3. Harness design

`benchmarks/host_bench.cpp`:

1. Probes the host.
2. Runs a **deterministic** integer calibration workload (`xorshift`-style
   mixing) with a known checksum, so a benchmark cannot silently measure a
   broken build.
3. Runs it `--repeats` times, **discards one warm-up sample**, and reports
   min/p25/median/p75/max/mean/stddev plus `iqr_fraction` (the noise indicator).
4. Guards against the optimiser eliminating the timed work: an `asm` barrier
   keeps the computation between the clock reads, and a **plausibility check**
   rejects a result faster than one clock cycle per iteration rather than
   reporting it. See BENCH-001 in `docs/known-issues.md`.
5. Exits non-zero if the checksum is unstable or the timing is implausible, so
   a broken measurement fails the build instead of being published.
6. Emits `guest_execution: { "available": false, "reason": ... }` until the
   translator exists.

Run it:

```bash
scripts/dev.sh bench --iterations 1000000 --repeats 9 --json
```

### JSON schema

```json
{
  "schema": "hyperion.bench.host.v2",
  "build": "hyperion 0.1.0 [release] gcc 14.2.0 c++202002",
  "host": { "arch": "...", "kernel": "...", "logical_cpus": 0,
            "page_size": 0, "cache_line_size": 0 },
  "calibration": {
    "iterations": 0, "repeats_requested": 0, "warmup_discarded": 1,
    "samples": 0,
    "ns_per_op_min": 0.0, "ns_per_op_p25": 0.0, "ns_per_op_median": 0.0,
    "ns_per_op_p75": 0.0, "ns_per_op_max": 0.0, "ns_per_op_mean": 0.0,
    "ns_per_op_stddev": 0.0, "iqr_fraction": 0.0,
    "checksum": "0x...", "checksum_stable": true,
    "measurement_plausible": true
  },
  "guest_execution": { "available": false, "reason": "..." }
}
```

The schema version is incremented on any breaking change to the JSON shape.
`v1` reported a single sample and no spread; `v2` reports the repeated-run
statistics the methodology requires.

---

## 4. Measured results

### 4.1 Host calibration — this development environment

**Measured 2026-10-19.** Host: AMD EPYC 9B14 (x86-64, KVM guest), Debian 13
(trixie), kernel 6.8.0-1055-gke, 4 logical CPUs, page size 4096 B, cache line
64 B. Build: `hyperion 0.1.0 [release] gcc 14.2.0`, RelWithDebInfo, `-O2`.

Command:

```bash
build/dev/benchmarks/hyperion_bench --iterations 1000000 --repeats 9 --json
```

| Statistic | ns/op |
| --- | --- |
| min | 2.055060 |
| p25 | 2.056132 |
| **median** | **2.056794** |
| p75 | 2.079814 |
| max | 2.138529 |
| mean | 2.075143 |
| stddev | 0.030362 |

`samples: 8` (9 requested, 1 warm-up discarded) · `iqr_fraction: 0.0115` ·
`checksum: 0x2d288a32c31afada` (stable) · `measurement_plausible: true`.

The spread is **1.2 %** of the median, so this host is quiet enough for
candidate-vs-baseline comparison at the granularity this project needs.

#### Scaling validation

A calibration figure is only trustworthy if it scales with the work requested.
A workload that is too fast for the iteration count, or that the optimiser
eliminated, would produce a constant total time. Measured:

| Iterations | Median ns/op | Total (s) |
| --- | --- | --- |
| 1,000,000 | 2.0633 | 0.002063 |
| 10,000,000 | 2.0575 | 0.020575 |
| 100,000,000 | 2.0573 | 0.205731 |

Total time scales linearly while ns/op stays flat, as it must for a real
measurement. This check is what exposed **BENCH-001** (see
`docs/known-issues.md`), where an earlier revision of this harness reported a
fabricated ~0.00003 ns/op because the optimiser hoisted the timed workload out
of the repeat loop. The regression is now guarded by the
`bench.timing_not_eliminated` test.

These are **calibration figures for the harness on a virtualized host**, not a
performance claim about HYPERION. They say nothing about translation throughput;
that is measured separately in 4.2.

### 4.2 Guest execution — Tier 0 vs Tier 1 baseline (Phase 5)

The translator exists now, so the guest-execution rows are measurable. The first
Phase 5 task was to record a **baseline before optimising**, exactly as the
roadmap requires.

**Harness.** `benchmarks/tier_bench.cpp` (target `hyperion_tier_bench`). It runs
one fixed guest program through both tiers with the shared methodology: one
discarded warm-up, repeated timed samples, median with IQR spread, and a
correctness check on the final `rdx` so a benchmark cannot measure a broken
translator.

**Workload.** `accumulate rax into rdx rcx times, halt`
(`31 D2 | 48 01 C2 | 48 FF C9 | 75 F8 | F4`), `rax=1`, `rcx=iterations`. Each
iteration retires 3 guest instructions (`add`, `dec`, `jnz`); the numbers below
use 200,000 iterations = 600,002 retired instructions.

**Environment.**

| Field | Value |
| --- | --- |
| Host arch | aarch64 (executed under `qemu-aarch64`) |
| Kernel | 6.8.0-1055-gke |
| Logical CPUs | 4 |
| Page size / cache line | 4096 / 32 bytes |
| Build | RelWithDebInfo, gcc, `HYPERION_BUILD_BENCHMARKS=ON` |
| Command | `scripts/bench-tier-qemu.sh --iterations 200000 --repeats 7` |
| Commit | Phase 5 baseline (this commit) |

**Result (median of 7 samples, 1 warm-up discarded):**

| Tier | ns median | ns / guest instruction | IQR / median | Correct |
| --- | --- | --- | --- | --- |
| Tier 0 (interpreter) | 295,117,038 | 491.86 | 0.018 | yes |
| Tier 1 (translator) | 23,041,650 | 38.40 | 0.008 | yes |

Ratio (Tier 1 / Tier 0): **0.078**, i.e. Tier 1 is about **12.8× faster** than
the reference interpreter on this workload.

**Interpretation, and what this does *not* say.**

- This is the ratio between *Hyperion's own two tiers*. It is a relative sanity
  signal: it confirms the translator's design (one translation reused across a
  hot loop, versus per-instruction interpretation) actually pays off.
- It is **not** a hardware performance claim. Under `qemu-user` both tiers are
  additionally translated by qemu, so absolute ns are meaningless and the ratio
  is distorted by qemu's treatment of the two very different code shapes.
- The reference target (Snapdragon 855 / Android 10) is still **unavailable**;
  see 4.3. Do not assume target performance from these numbers.
- Tier 1 is still very slow in absolute terms: no register allocation and no
  block chaining, so each guest instruction becomes a load/compute/store of
  `GuestState` and every block exit returns to a hash lookup. That is the
  starting point Phase 5 optimises from.

**Reproduce:**

```
scripts/bench-tier-qemu.sh --iterations 200000 --repeats 7
```

#### 4.2.1 Optimization 1 — parity fast path (Phase 5)

**Hypothesis.** Each guest ALU instruction that writes flags emits ~39 AArch64
instructions, of which ~19 are the parity (PF) computation: a per-bit fold plus
its setup. Replacing the bit-by-bit accumulation with a XOR-fold (8→4→2→1 bits)
using AArch64 shifted-register `EOR` cuts that block to 6 emitted instructions.

**Change.** `emit_parity` in `src/core/translator.cpp`; new encoder primitive
`Encoder::eor_shifted`. Encodings verified against `aarch64-linux-gnu-as`
(all four shift types) before use — the first attempt wrongly set bit 21, which
the new encoder test caught.

**Evidence.** Timing under `qemu-user` is noisy and the absolute ns drift
between runs, so the primary evidence is *deterministic*: the generated-code
size for the fixed workload, measured with `TranslationCache::total_code_bytes`.

| Metric | Baseline (`f04c659`) | After parity fast path | Change |
| --- | --- | --- | --- |
| Generated AArch64 bytes (same workload) | 904 | 644 | **−28.8%** |
| Tier 1 ns / guest instruction (interleaved A/B) | 38.20 | 31.50 | −17.5% |
| Tier 1 / Tier 0 ratio (interleaved A/B) | 0.0817 | 0.0733 | −10.3% |

The A/B is *interleaved*: the baseline and current binaries were built from the
same tree state (both `RelWithDebInfo`, `-O2`) and run alternately five times,
so machine drift cancels. Both tiers are correct in every run.

**The delta is fully explained, which is the strongest evidence here.** The
workload retires five flags-writing guest instructions per loop trip (`xor`,
`add`, `dec`, `add`, `dec`). Each parity rewrite removes exactly 13 AArch64
words (19 → 6). Five × 13 = 65 words = 260 bytes, which is precisely the
measured 904 − 644 delta. No other part of the code generator changed size, so
the optimization does exactly what it claims and nothing else.

**Interpretation, and limits.**

- The deterministic −28.8% code size is the trustworthy number. The 17.5%
  wall-clock gain is consistent with it but should not be over-read: the whole
  workload is dominated by `GuestState` load/store, so shrinking the flags
  prologue helps less than the instruction count alone suggests.
- This does **not** change the conclusion of 4.2: Tier 1 is still ~13× the
  interpreter in this workload and still very slow in absolute terms. The next
  structural wins (register allocation, block chaining) are untouched.

**Regression guard.** `translator.translated_code_size_is_bounded` pins the
block count and an upper bound on generated words for a fixed loop, so a future
code-generator change that silently bloats output fails CI even when timing
noise would hide it. The exact byte count is reported by the benchmark
(`tier1_translator.generated_code_bytes`).

**Reproduce:** `scripts/bench-tier-qemu.sh --iterations 200000 --repeats 15`.

#### 4.2.2 Optimization 2 — dead `rip` stores (Phase 5)

**Hypothesis.** Every translated body instruction ended with
`movz`+`movk`+`str` to advance `GuestState.rip`. But nothing *inside* a block
reads `rip`: only the dispatcher does, after the block returns. Every
intermediate store is therefore dead, and a block needs exactly one `rip` write
at its exit.

**Change.** `emit_one` gained a `write_rip` parameter. Body instructions in a
block are emitted with `write_rip=false`; the block writes `rip` once, at
whichever exit it takes (fall-through, `jmp`, `jcc`, `hlt`). The standalone
single-instruction translator (`translate_instruction`) still passes
`write_rip=true`, because that path is a whole "block" of one instruction and
the caller reads `rip` back.

**Evidence.**

| Metric | Before (`b3dbcb6`) | After | Change |
| --- | --- | --- | --- |
| Generated AArch64 bytes (same workload) | 644 | 584 | **−9.3%** |
| Tier 1 ns / guest instruction (interleaved A/B) | 31.3 | 29.8 | ≈ −4% |

Again the delta is fully accounted for: the benchmark loop has five body
instructions (two `add`, two `dec`, one `xor`), each dropping a 3-instruction
`rip` write = 15 words = 60 bytes = exactly 644 − 584.

**Correctness.** The differential suite compares the *final* `GuestState`,
including `rip`, against the interpreter, so a missing or misplaced `rip` write
is a hard failure. The single-instruction differential tests still cover the
`write_rip=true` path. Cross suite green under `qemu-aarch64` (99 passed, 0
failed).

#### 4.2.3 Optimization 3 — flag liveness (Phase 5)

**Hypothesis.** Every ALU instruction that writes flags emitted the full
six-flag prologue (~29 AArch64 instructions: `cset`+`strb` per flag, the AF
`eor` identity, and the parity fold), regardless of whether anything ever read
those flags. Most flags in a real block are dead: a later instruction overwrites
them before any `jcc` reads them. Computing a dead flag is pure overhead — it
neither changes the result nor is observable.

**Change.** `translate_block` became a three-pass function
(`src/core/translator.cpp`):

1. Decode the whole block into a list (unchanged block-cutting rules).
2. Backward flag liveness over that list: `live_out` starts at *all* flags at
   the block exit (flags persist in `GuestState` across blocks, so a successor
   may read any of them — there is no cross-block analysis), and each
   instruction's live set is `writes & live_out`; `live_out` then becomes
   `reads | (live_out & ~writes)`.
3. Emit, passing each instruction's live mask.

`emit_flags_from_nzcv`/`emit_flags_logical` now store only the masked flags, and
the AF `a^b`/`a^b^r` identity is materialised only when AF is live. INC/DEC
gained a second, independent win: because `flags_written(INC/DEC)` excludes CF,
CF is never stored for them, so the explicit load-CF/restore-CF pair (2
instructions per INC/DEC) disappeared. That is a correctness improvement as much
as a speed one — the previous save/restore was the only thing preserving CF, and
it is now preserved structurally.

**Evidence.**

| Metric | Baseline (`250a148`) | After flag liveness | Change |
| --- | --- | --- | --- |
| Generated AArch64 bytes (fixed workload) | 584 | 352 | **−39.7%** |
| AArch64 words, `translated_code_size_is_bounded` program | 148 | 90 | −39.2% |
| Tier 1 ns / guest instruction (under `qemu-user`) | 29.7 | 29.7 | ≈ 0% |

**The delta is fully accounted for, and the wall-clock result is honestly
null.** The benchmark loop body is `add rdx,rax | dec rcx | jnz` (plus the entry
`xor edx,edx`). The `xor` and `add` have all six flags dead (the `jnz` reads only
ZF, and both are overwritten by the following `dec`), so each loses the entire
flag prologue; the `dec` is left computing only ZF/SF/PF/OF (CF and AF dead).
That accounts for the 584 → 352 byte drop. But under `qemu-user` the *timing* is
unchanged: the loop is dominated by `GuestState` load/store and qemu's own
translation, and removing ~40% of AArch64 instructions in a region qemu has
already translated to host code does not move qemu's host execution time here.
**The −39.7% code size is real and deterministic; the wall-clock gain is not
demonstrated by this measurement and is not claimed.** A direct AArch64 host
would be the right place to measure it; that hardware is unavailable (4.3).

**Interpretation, and limits.**

- Code size is the trustworthy number, and it is a proxy for the work a real
  CPU would do (fewer instructions, fewer `strb`s to the `GuestState`). It is
  not itself a speedup claim.
- The optimization is conservative: `live_out = all` at block exit means a flag
  written as the last instruction of a block is still always computed. That is
  required for cross-block correctness and is the safe default.
- Correctness is guarded by the differential suite, which compares the *final*
  `GuestState` — including all six flags — against the interpreter, plus the new
  `translator.dead_flags_are_not_computed` test (a live `add` costs ≥15 words, a
  dead one ≤4).

**Reproduce:** `scripts/bench-tier-qemu.sh --iterations 200000 --repeats 7`.

#### 4.2.4 Optimization 4 — direct-mapped dispatch (Phase 5)

**Hypothesis.** After every block exit the dispatcher resolves the next guest PC
through an `std::unordered_map`, which is a hash computation plus a pointer
chase per block. A hot loop re-enters a handful of PCs millions of times, so a
small direct-mapped table indexed by the low bits of the PC should answer almost
every lookup with one array access.

**Change.** `TranslationCache` gained a 4096-entry direct-mapped table
(`direct_`) plus `lookup()`/`install()` (`include/hyperion/translation_cache.hpp`,
`src/core/translation_cache.cpp`). `lookup()` checks `direct_[pc & 0xFFF]` and
only falls back to the hash map on a slot mismatch. **The hash map remains
authoritative**: a slot is used only if `block->start == pc`, so a collision
(a different PC in the same slot) is a safe fall-through to the map, never a
wrong block. The fast path therefore carries no correctness risk by construction.

**Evidence (deterministic — dispatch counters, not timing).**

| Metric | Value |
| --- | --- |
| Dispatch hits | 199,998 |
| Dispatch misses | 3 |
| Hit rate | 99.9985% |
| Generated AArch64 bytes | 352 (unchanged) |

The 3 misses are exactly the first sighting of each of the 3 blocks; every
subsequent dispatch is served by the direct table. This is a *dispatch-overhead*
reduction: it removes a hash lookup from the per-block cost. As with 4.2.3 the
wall-clock effect under `qemu-user` is not measurable here (qemu has already
translated both paths to host code) and is not claimed; the counters are the
honest evidence.

**Correctness.** `translation_cache.direct_map_collision_falls_back_to_map`
forces two PCs (`0x400000` and `0x401000`) into the same slot and checks that all
three runs return the right value with only two translations. The counters are
exposed in the benchmark JSON (`dispatch_hits`, `dispatch_misses`).

**Reproduce:** `scripts/bench-tier-qemu.sh --iterations 200000 --repeats 7`.

#### 4.2.5 Optimization 5 — ALU immediate folding (Phase 5)

**Hypothesis.** An `add/sub/cmp reg, imm` whose immediate fits the AArch64
ADD/SUB 12-bit immediate form was lowered as *materialise the constant into a
register, then ADDS/SUBS register-register*. The materialisation (`mov_imm64`) is
1-4 instructions, so the immediate form is a pure instruction-selection win.

**Change.** `emit_one` folds a foldable immediate straight into `adds_imm`/
`subs_imm` (new `encode_imm12` helper), skipping `load_source`. The fold is
applied **only when AF is not live**: AF is derived from `a ^ b ^ r` and needs
`b` in a register, so a live-AF instruction keeps the register form to stay
bit-exact. This is the honesty rule in action — the differential test
(`differential_mov_and_alu`) caught a first attempt that folded unconditionally
and got AF wrong, and the fold was narrowed until the test passed.

**Evidence.** Deterministic code size for the size-guard program:

| Metric | After 4.2.3 | After immediate folding | Change |
| --- | --- | --- | --- |
| AArch64 words, `translated_code_size_is_bounded` | 90 | 88 | −2 |

The benchmark loop workload uses a register-form `add`, so it is unchanged at
352 bytes; the win shows on immediate-heavy code. The −2 words are exactly the
`mov`+`movk` that `add rdx, 1` no longer emits.

**Interpretation, and limits.**

- Small, and honestly reported as small. It is included because it is a clean,
  isolated instruction-selection change with a dedicated test
  (`translator.alu_immediate_is_folded`), not because it moves the headline.
- The AF guard means the common "compare against a small constant then branch"
  pattern (`cmp r, imm`; `jcc`) still materialises when the branch reads AF —
  but branches read ZF/CF/SF/OF, not AF, so in practice `cmp r, imm; jcc` folds.
- As with 4.2.3/4.2.4, no wall-clock claim under `qemu-user`.

**Reproduce:** `scripts/bench-tier-qemu.sh --iterations 200000 --repeats 7`.

#### 4.2.6 Optimization 6 — guest-register allocation within a block (Phase 5)

**Hypothesis.** Every guest register access lowered to a load or store of
`GuestState` in memory, so `add rbx, rax` was `ldr/ldr/add/str`. Keeping the
block's hot guest registers in **callee-saved host registers** (x19..x28) for the
duration of the block should remove most of that traffic.

**Change.** A new pass (1.5) counts, per guest register, how many instructions
reference it in the block, ranks registers by that count, and maps the top ones
to x19..x28. A save/restore prologue/epilogue wraps the block (paired `stp`/`ldp`
for the borrowed callee-saved registers, plus one `ldr` at entry and one `str` at
exit per allocated register). `kMov`, the ALU ops and `inc`/`dec` were rewritten
to operate directly on the allocated host register when both operands are
allocated. `GuestState` remains authoritative at every block boundary, so the
block signature and the cross-block flag invariant are unchanged.

**Profitability guard — measured, not assumed.** The first implementation
allocated *every* touched register. That **regressed** the code size of the
3-instruction benchmark loop (364 → 388 bytes) because the prologue/epilogue
outweighed the saved loads for a tiny block, and it was neutral-to-slower in
time. The allocator was therefore made *profitability-aware*: a register is
allocated only when its body memory-op count exceeds the step increase in
overhead (`alloc_cost(n) = 2n + 2*ceil(n/2)`). With the guard, the narrow loop
correctly declines allocation for its hot block and the code size comes out at
356 bytes (RA on) vs 364 (RA off).

**A second workload was required to measure the win.** The narrow loop's hot
block is only 3 instructions, so allocation never fires there; it cannot show
what RA is worth. A `--wide` workload was added to `hyperion_tier_bench`: a
12-instruction loop body that touches **eleven** guest registers (more than the
ten allocatable host registers), so the allocator must choose.

**Evidence — wide workload, `--iterations 100000 --repeats 7`:**

| Metric | RA off | RA on | Change |
| --- | --- | --- | --- |
| Generated code bytes | 620 | 608 | −1.9% |
| Tier 1 ns / guest instruction | 10.70 | 9.05 | −15.4% |
| Blocks | 2 | 2 | — |
| Correct | yes | yes | — |

The −15% tier-1 time is reproducible across repeated runs (9.07, 9.01 ns on
separate runs) and is the direction the change predicts: fewer memory ops per
instruction in the hot block. As always under `qemu-user`, this is a *relative*
signal — qemu additionally translates both tiers — not a hardware claim.

**Correctness.** Two differential tests were added:

- `translator.differential_register_allocation_many_registers` — an
  eleven-register program with **distinct** values per register (all-zero would
  not discriminate a misallocated register), checked instruction-by-instruction
  against the interpreter via `check_program_matches`.
- `translator.differential_register_allocation_immediates` — an allocated
  destination written by a `mov` immediate and then read by a later op.

Adding the first test **exposed a pre-existing correctness bug**: when a block
ended in `HLT` after body instructions, the generated code never wrote `rip`, so
the halted `rip` leaked the block's *start* address instead of the `HLT`
address (the interpreter leaves `rip` on the `HLT`). `check_program_matches` had
always compared `rip`, but no earlier program ended a multi-instruction block in
`HLT`, so it went unnoticed. Fixed by emitting `rip = insn.address` on the halt
path (BENCHMARKS.md 4.2.7).

**Interpretation, and limits.**

- The win is real but narrow: it is a per-block allocation with no live-range
  splitting, no cross-block allocation and no spilling heuristics beyond "stop
  when it no longer pays". A register used across a block boundary is still
  reloaded.
- The profitability model is a static instruction count, not a measured cost;
  it is deliberately conservative (it prefers to *not* allocate when unsure),
  which is why the narrow loop is untouched.
- No wall-clock claim under `qemu-user`.

**Reproduce:** `scripts/bench-tier-qemu.sh --wide --iterations 100000 --repeats 7`
and, for the code-size figures, the size guard in `translated_code_size_is_bounded`.

#### 4.2.7 Correctness fix — halted `rip` was the block start (Phase 5)

**Bug.** Tier 1's block epilogue for a normal exit advanced `rip`, but a block
ending in `HLT` (or `RET`) returned `kHalted` **without** writing `rip`. The
dispatcher then reported `state.rip` unchanged from block entry, i.e. the block's
first address, whereas the interpreter leaves `rip` on the `HLT` instruction.

**Impact.** Any guest program that halts after more than one instruction in its
final block (i.e. almost all of them) reported the wrong halted `rip`. The
`rdx`-only checks in earlier benchmarks and tests never looked at `rip`, so it
went unnoticed until the RA differential test compared full state.

**Fix.** Emit `emit_set_rip(e, step.insn.address)` on the `HLT`/`RET` path before
`emit_return(kHalted)`. Cost: 3 AArch64 words on the halt block; the size guard
moved 88 → 91 with RA off, back to 88 with RA on (the profitability guard's −3
offsets it exactly).

**Test.** `translator.differential_register_allocation_many_registers` (the
program that caught it) now locks the regression out; every other
`check_program_matches` program compares `rip` as well.

#### 4.2.8 Phase 5 Optimization Pass — `ExecArena`, 2-Way Chaining, Hardware `NZCV` Fusion, Self-Loop Superblocks, and Tier 0 Decoded ICache

- **Date / commit:** 2026-10-21 / `921c7bd` (compared against unmodified `03487fc` on the identical host, compiler, flags, and `--iterations 200000 --repeats 7` invocations)
- **Host:** Linux 6.1.158+ x86_64, GCC 14.2.0 `RelWithDebInfo`, `qemu-aarch64` 10.0.13
- **Commands:**
  - `./build/dev/benchmarks/hyperion_tier_bench --iterations 200000 --repeats 7 --json`
  - `./build/dev/benchmarks/hyperion_tier_bench --wide --iterations 200000 --repeats 7 --json`
  - `./scripts/bench-tier-qemu.sh --iterations 200000 --repeats 7 --json`
  - `./scripts/bench-tier-qemu.sh --wide --iterations 200000 --repeats 7 --json`

| Workload (`200,000` iters, `7` repeats, 1 warm-up discarded) | Baseline (`03487fc`) | Optimized (`921c7bd`) | Speedup / Change |
| --- | ---: | ---: | ---: |
| **Native x86-64 Tier 0 — Narrow (`600,002` insns)** | `24,831,247 ns` (`41.385 ns/insn`) | `7,146,357 ns` (`11.911 ns/insn`) | **3.47× faster** |
| **Native x86-64 Tier 0 — Wide (`2,400,002` insns)** | `110,085,761 ns` (`45.869 ns/insn`) | `37,994,884 ns` (`15.831 ns/insn`) | **2.90× faster** |
| **`qemu-aarch64` Tier 0 — Narrow (`600,002` insns)** | `392,312,068 ns` (`653.851 ns/insn`) | `117,819,894 ns` (`196.366 ns/insn`) | **3.33× faster** |
| **`qemu-aarch64` Tier 0 — Wide (`2,400,002` insns)** | `1,624,420,613 ns` (`676.841 ns/insn`) | `517,432,047 ns` (`215.597 ns/insn`) | **3.14× faster** |
| **`qemu-aarch64` Tier 1 — Narrow (`600,002` insns)** | `21,337,245 ns` (`35.562 ns/insn`, `356 B` code) | `1,906,672 ns` (`3.178 ns/insn`, `344 B` code) | **11.19× faster** (`−3.4%` code) |
| **`qemu-aarch64` Tier 1 — Wide (`2,400,002` insns)** | `25,038,216 ns` (`10.433 ns/insn`) | `2,244,315 ns` (`0.935 ns/insn`, `624 B` code) | **11.16× faster** (`230.6×` over Tier 0) |

- **Correctness:** 100% pass across all native CTest suites, Clang Debug build, ASan + UBSan build, and AArch64 cross-tests (`translator.translated_code_size_is_bounded` <= 88 words preserved; exact `steps` and `dispatch_hits` accounting verified with and without `max_steps`). Note per §5 rule 5: `qemu-aarch64` numbers measure relative improvements on the identical host rather than Snapdragon 855 silicon timing.

#### 4.2.9 Full-System AArch64 + 128-bit ARMv8 NEON Tier 1 JIT & Real Compiled `glibc 2.41` Binary Evaluation

- **Date:** 2026-10-21 (compared apples-to-apples on the identical Linux `6.1.158+` `x86_64` host, `GCC 14.2.0` `RelWithDebInfo`, `glibc 2.41`, `qemu-aarch64` `10.0.13`)
- **Commands:**
  - `qemu-aarch64-static ./build/cross-bench/benchmarks/hyperion_tier_bench` (`N = 5`, 1 warm-up discarded, `3,000,002` retired guest instructions)
  - `./scripts/run_real_benchmarks.sh` (`N = 5`, 1 warm-up discarded, 5 real `gcc 14.2 -O2 -static` C binaries totaling `120,313,691` dynamic x86-64 + SSE2 instructions)

| Workload (`N = 5`, 1 warm-up discarded) | Guest Insns | Tier 0 (`qemu-aarch64`) Median | **Tier 1 ARMv8+NEON JIT (`qemu-aarch64`) Median** | **Speedup vs Tier 0** |
| --- | ---: | ---: | ---: | ---: |
| **`hyperion_tier_bench` (loop kernel)** | `3,000,002` | `797.05 ms` (`265.68 ns/insn`) | **`7.63 ms` (`2.54 ns/insn`, `393.3 MIPS`)** | **`104.49×`** |
| **`sieve` (`1,000,000` primes, `glibc 2.41`)** | `16,990,070` | `4,665.15 ms` | **`903.06 ms` (`18.81 MIPS`)** | **`5.17×`** |
| **`sha256` (`256 KiB` auto-vectorized SSE2)** | `20,682,677` | `5,792.50 ms` | **`287.84 ms` (`71.86 MIPS`, `13.92 ns/insn`)** | **`20.12×`** |
| **`qsort_glibc` (`50,000` `int64` `glibc` `qsort`)** | `21,606,066` | `6,450.06 ms` | **`864.64 ms` (`24.99 MIPS`)** | **`7.46×`** |
| **`quicksort_c` (`100,000` `int32` Hoare sort)** | `21,672,163` | `6,046.26 ms` | **`539.53 ms` (`40.17 MIPS`)** | **`11.21×`** |
| **`nqueens` (`N = 12` bitboard recursion)** | `39,362,715` | `10,722.29 ms` | **`633.08 ms` (`62.18 MIPS`, `16.08 ns/insn`)** | **`16.94×`** |
| **Total across 5 full static `glibc 2.41` binaries** | **`120,313,691`** | **`33,676.26 ms`** | **`3,228.15 ms` (`37.27 MIPS` overall)** | **`10.43×`** |

- **Native x86-64 Tier 0 single-core throughput (`N = 5`):** `55.79`–`60.67 MIPS` (`16.48`–`17.92 ns/insn`, `2,069.70 ms` total across all `120,313,691` instructions).
- **Correctness:** Exact `0`-step discrepancy (`x86_64 Tier 0 == aarch64 Tier 0 == aarch64 Tier 1`) and 100% bit-exact output across all `120,313,691` dynamic instructions; see [`docs/real-world-evaluation.md`](docs/real-world-evaluation.md).

### 4.3 Reference target device (Snapdragon 855)

**unavailable.** The reference device is not present in this environment.
Target-device measurements require real hardware and will be recorded in
`docs/target-environment.md` and here when available. Do not assume target
performance from x86-64 host numbers.

### 4.4 Canonical cross-block register allocation (shipped)

Every cached block now uses one fixed guest→host mapping — `rax, rcx, rdx,
rsp, rsi` in `x19..x23` (audit §29.27, §29.29) — loaded once by the entry
trampoline per JIT entry and stored only where control returns to C++
(halt/syscall/fault/helper funnels, dispatcher miss, budget exhaustion).
JIT-internal exits (linked branches, chained dispatch, call/ret) no longer
spill or reload guest registers at block boundaries.

Measured on this host under qemu-user (candidate/baseline execution time,
`--timing` wall minus translation; 36 interleaved pairs per guest across
three sessions with binary order rotated; A/A identical-binary controls
spanned 0.984–1.014):

| guest | median ratio | bootstrap 95 % CI |
| --- | --- | --- |
| `sieve` | 0.970 | [0.953, 0.982] |
| `nqueens` | 0.989 | [0.976, 1.008] |
| `quicksort_c` | 0.951 | [0.920, 0.976] |
| `qsort_glibc` | 1.002 | [0.992, 1.014] |

Two clear gains, one small gain, one flat; no regression beyond rig noise.
Translation time is unchanged. The cost is static: generated code grows
8–9 % (e.g. sieve 171,712 → 187,036 bytes), and the pinned code-size test
moves 88 → 89 words for its 3-block program. Guest-visible output is
byte-identical on all four workloads; the full test suite passes natively
and under qemu (419 passed / 0 failed / 5 skipped), including two new
regression tests for canonical values crossing block boundaries and for
state synchronisation on a budget-exhaustion exit.

### 4.5 Canonical XMM residency (shipped)

Guest `xmm0..7` now live in host `q16..q23` across JIT-internal block
boundaries (audit issue #17), under the same contract as the canonical
GPRs: loaded once by the entry trampoline, stored by the shared exit
epilogue (the single C++ funnel), spilled/reloaded around Tier-B helper
calls, and preserved around SoftTlb slow-path C++ calls by one shared
generated trampoline (`ctx->jit_preserve_call`) so each slow stub costs
one extra word instead of an 8-word spill/reload pair. A pre-change
traffic census (tier-0 instrumented simulation of the tier-1 emission
model) counted 8.17M GuestState XMM accesses in `sha256`
(0.40/instruction); residency removes ~99 % of them. `xmm8..15` stay
memory-resident.

Measured on this host under qemu-user (candidate/baseline execution
time, 36 interleaved pairs per guest, order rotated):

| guest | median ratio | bootstrap 95 % CI |
| --- | --- | --- |
| `sha256` | 0.832 | [0.818, 0.841] |
| `sieve` | 0.982 | [0.966, 1.005] |
| `nqueens` | 0.995 | [0.969, 1.011] |
| `qsort_glibc` | 0.996 | [0.981, 1.010] |
| `quicksort_c` | 1.017 | [1.004, 1.023] |

The SSE-dense workload is ~17 % faster; three guests are flat.
`quicksort_c` shows a consistent ~1.7 % cost on this rig despite its
hot generated code being byte-identical (its SSE traffic is ~800
instructions; every added dynamic operation on its paths measured
negligible at ~8K JIT re-entries/run) - attributed to generated-code
layout sensitivity of the qemu TCG translator, and accepted against
the SSE win. Static generated code grows ~2 %; the pinned code-size
test moves 89 -> 105 words (the trampoline/dispatcher sync words).
Translation time is unchanged and guest-visible output is
byte-identical on all five workloads. Two regression tests pin
resident values across block boundaries and their synchronisation on
a budget-exhaustion exit.

---

## 5. Methodology for future phases

1. **Fixed baseline.** Optimization candidates are compared against a frozen,
   committed baseline build; the baseline is never silently changed (Phase 8).
2. **Repeated runs.** Report median and spread, not a single sample; discard a
   warm-up run.
3. **Pinned environment.** Record CPU governor/affinity where controllable;
   note when it is not.
4. **Correctness gate.** A faster result is void unless the correctness suite
   passes on the same build.
5. **No cross-machine comparison.** qemu-user numbers are correctness evidence
   only; performance comparisons require the same physical machine.
6. **Attribution.** Every row links to the commit SHA and the JSON report.

---

## 6. Reporting template

```
### <benchmark name>
- Date / commit:
- Host (arch, kernel, CPU, RAM, page size, cache line):
- Compiler / build type / flags:
- Command:
- Baseline result:
- Candidate result:
- Correctness: <pass/fail, test names>
- Verdict: accept | reject | inconclusive
- Notes:
```
