# HYPERION dynamic translator — code-level audit

**Date:** 2026-10-25 · **Commit:** `7a370a6` (`main`, CI green) · **Method:** source reading plus
execution of the existing tests and benchmarks. No source file was modified.

This document answers 28 specific questions and then sections A–L. Every claim is tagged with one of
five states, and the tag matters more than the prose:

| Tag | Meaning |
| --- | --- |
| **implemented + tested** | Code exists, and a test executes it and would fail if it broke. |
| **implemented, weak** | Code exists and runs, but the implementation is known-poor or has a measurable flaw. |
| **implemented, unreachable** | Code exists but cannot execute in the environment under test (or at all, from the CLI). |
| **partial** | Covers a subset of the stated feature; the rest is refused or absent. |
| **documented only** | Named in prose, absent from the execution path. |

---

## 0. What environment this was measured on — read this first

**This sandbox is x86-64 Linux (Debian, kernel 6.1.158), not ARM64 Termux.** `uname -m` → `x86_64`.
That single fact determines what could be verified:

* On x86-64, `ExecMem::executable_supported()` is hard-coded `false`
  (`src/core/exec_mem.cpp`, `#else return false;`). The CLI therefore prints
  *"this host cannot execute generated AArch64 code; falling back to the reference interpreter"*
  (`src/cli/main.cpp:164,250`) and every `hyperion run` here is **Tier 0**.
* To exercise Tier 1 at all I cross-compiled the tree for AArch64 (`gcc 14.2.0`) and ran it under
  `qemu-aarch64-static 10.0.13`. All Tier 1 numbers below are therefore **qemu results**: they
  establish *relative* behaviour and correctness, never absolute performance, and never hardware
  characteristics (SELinux `PROT_EXEC`, 16 KB pages, real cache hierarchy) — none of which exist here.

So: nothing in this report was measured on ARM64 hardware, because none is reachable.

### Commands actually run

```
# native x86-64 (Tier 0 only)
build-strict/tests/hyperion_unit_tests                       -> 337 passed, 0 failed, 24 skipped (361)
build-strict/benchmarks/hyperion_tier_bench                  -> tier 1: unavailable (host cannot execute AArch64)
build-strict/benchmarks/hyperion_syscall_bench --iterations 300000 --repeats 5
build-strict/hyperion run --tier=0 --quiet /tmp/sysloop      -> 744 ns per getpid (100k calls)

# AArch64 cross build under qemu-user (Tier 0 and Tier 1 both run here)
build-aarch64/tests/hyperion_unit_tests                      -> 360 passed, 0 failed, 5 skipped (365)
build-aarch64/benchmarks/hyperion_tier_bench --iterations 200000 --repeats 5
build-aarch64/benchmarks/hyperion_syscall_bench
build-aarch64/hyperion run --tier={0,1} /tmp/sieve 200000
build-aarch64/hyperion run --tier={0,1} /tmp/tiny
```

---

## 1. Is HYPERION genuinely doing dynamic binary translation, or is part of the path still interpretation?

**Answer: it is a genuine DBT for straight-line integer code, and a hybrid interpreter for the rest.**

Three execution modes exist, and all three are real code:

1. **Tier 0** — a plain interpreter with a 8192-slot decoded-instruction cache
   (`src/core/interpreter.cpp:3057` `run_program`, `:3067` `kDecodeCacheSize = 8192`). This is what
   runs on x86-64 and on any AArch64 host that cannot get `PROT_EXEC`.
2. **Tier 1** — real AArch64 machine-code generation (`src/core/translator.cpp`, 3436 lines),
   executed from `mmap`ed RX memory, with in-JIT block chaining. **implemented + tested** (on AArch64).
3. **Dynamic selection between them** — **documented only / partial**. The tier is a *static CLI flag*
   (`--tier=`, default 1) with a one-way capability fallback. There is no per-block promotion, no
   hot-block detection, and, critically, **no interpreter fallback inside Tier 1**: a block that
   fails to translate aborts the whole run with `kUnsupported`
   (`src/core/translation_cache.cpp:184-190`). So it is not "interpreter that warms into a JIT"; it is
   "pick one, and stay there".

Within Tier 1 there is however a second, smaller interpretation mechanism: **approximately 15% of
opcodes are not translated at all** but executed by calling the interpreter's C++ helper from inside
the generated block (`Tier B` path, `src/core/translator.cpp:3004-3023`). That is deliberate (it keeps
the block unfragmented) but it means the generated code frequently calls back into C++.

Verdict: **genuine DBT, hybrid coverage, no tiering.** The word "dynamic" in "dynamic binary
translation" is accurate in the sense of "translate at run time"; it is *not* accurate in the sense of
"adaptive/promoting".

---

## 2. What exactly is Tier 0, and where does its hot path spend time?

Pipeline, one guest instruction per loop iteration (`src/core/interpreter.cpp:3074-3110`):

```
for i in 0..max_steps:
    pc    = state.rip
    epoch = memory.code_epoch()                      # virtual call
    if epoch != tlb.epoch: tlb.flush(epoch)          # 16 KiB rewrite
    slot  = &icache[pc & 8191]                       # direct-mapped
    if slot.pc != pc or slot.epoch != epoch:         # miss = fetch + decode
        fetch up to 15 bytes (retry 15,14,...,1)     # up to 15 virtual reads
        decode(...)                                  # ~2262-line decoder
    execute_decoded(state, memory, &tlb, slot.insn, syscalls)   # giant switch
```

Every major overhead in this loop:

| # | Overhead | Where | Cost |
| --- | --- | --- | --- |
| 1 | One **virtual call** to `memory.code_epoch()` per guest instruction | `:3076` | ~2-5 cycles, blocks inlining |
| 2 | `execute_decoded` is one ~800-line `switch` over ~60 opcodes; no computed-goto/threaded dispatch | `:2265` | branch mispredicts on mixed code |
| 3 | `width_of(insn.dst.size)` recomputed per instruction | `:2268` | trivial but repeated |
| 4 | **All six x86 flags computed eagerly** on every flag-writing ALU op, even when dead (`alu::adc`, `:107-120`: five flag computations + `__builtin_parity`) | `alu::` namespace | ~10-20 cycles per ALU op |
| 5 | Every guest memory access goes through a **function call** `fast_mem_read`/`fast_mem_write` with a `memcpy` of 1/2/4/8 bytes and a TLB probe that re-derives the slot each time | `:fast_mem_read` | ~15-25 cycles per access |
| 6 | **A full 16 KiB SoftTlb flush after every syscall** (1024 entries × 16 B), unconditionally, even for `write`/`getpid` | `:2320` | measured ~600 ns — see §16 |
| 7 | Operands are re-read from `Instruction` (`op.kind`, `op.mem.base`, …) per execution; no pre-decoded closure | throughout | extra loads/branches |
| 8 | The `icache` is a `std::vector` allocated and **zero-initialised per `run_program` call** (8192 × 24 B = 196 KiB) | `:3069` | one-off ~20 µs |

Measured on this x86-64 host: **15.85 ns per retired guest instruction ≈ 63 MIPS** for a tight
register-only loop. That is roughly 45 cycles per instruction — slow by interpreter standards (a good
one is 5–10), and consistent with items 1–5 dominating.

Tier 0 is **implemented + tested** (397 lines of `test_interpreter.cpp`, plus the 11-case hardware
differential suite). It is **correct-first, speed-last** by design.

---

## 3. What exactly is Tier 1? Is it a real optimizing JIT?

**Yes to "real JIT", no to "optimizing JIT".** It is a *baseline* block compiler with three
local optimizations.

What it does, per block (`translate_cached_block`, `src/core/translator.cpp:2102`):

1. **Discover** the block by decoding linearly from `pc` until the first `Jmp`/`Jcc`/`Call`/`Ret`/
   `Syscall`/`Hlt`, or 256 instructions.
2. **Pass 1.5 — block-local register allocation** into 5 callee-saved registers (`x19..x23`), chosen
   by simple reference counting, no liveness, no spilling (`:2163-2192`).
3. **Pass 2 — backward flag liveness**: compute exactly which of the 6 flags each instruction must
   materialise, so dead flag writes are not emitted (`:2194-2203`).
4. **Emit** AArch64: `emit_cached_body_insn` inline for ~31 opcodes and 42 of 115 SSE ops, and a
   `blr` into the interpreter helper for everything else (`:3004`).
5. **Fuse** `ALU + Jcc` into hardware `NZCV` + `csel` and emit a **single straight-line host basic
   block** with no internal host branches (`:3291-3345`).
6. **Self-loop superblock**: if the block's terminating `Jcc` targets its own start and the preceding
   ALU is fusible, emit a real AArch64 loop with a 2-instruction backedge (`:3033`, `:3152-3168`).

The three real optimizations are: *dead-flag elimination*, *NZCV condition fusion*, and *self-loop
superblocks*. There is no CSE, no strength reduction, no constant folding beyond immediate encoding, no
loop-invariant hoisting, no cross-block register allocation, no scheduling, no peephole pass, and no
IR at all — it emits AArch64 directly from the decoded `Instruction`.

**Selection:** `--tier=1` (the default) at the CLI. Reachability:

* x86-64 host → **unreachable**, permanently (`executable_supported()` returns `false`).
* AArch64 host → reachable, and it is what you get by default.
* AArch64 host where `mprotect(RX)` is denied (some Android SELinux domains) → falls back to Tier 0
  with an explicit stderr line (`exec_mem.cpp:110-125`). **implemented + tested** (`test_exec_mem.cpp`).

So the honest summary: Tier 1 is a working baseline JIT that is **unreachable on 100% of x86-64 hosts**
and reachable only on AArch64.

---

## 4. How are x86-64 registers mapped onto AArch64?

Fixed partition, no dynamic remapping (`src/core/translator.cpp:33-40`, `:1464-1469`):

| AArch64 | Role | Persistence |
| --- | --- | --- |
| `x8` | `GuestState*` (`kState`) | reloaded from `x27` after every C++ call |
| `x9..x15` | scratch: `kA`, `kB`, `kR`, `kT0..kT3` | clobbered freely within an instruction |
| `x19..x23` | **5 allocated guest GPRs** (`kAllocBase = 19`, `kCachedMaxAlloc = 5`) | live for one block only |
| `x24` | `jit_dispatch` (in-JIT block dispatcher) | entry trampoline → exit |
| `x25` / `x26` | initial / remaining step budget | entry → exit |
| `x27` | `GuestState*` mirror | entry → exit |
| `x28` | `ExecContext*` (SoftTlb + jump table) | entry → exit |
| `x0..x7` | helper-call arguments only | never allocated |

Guest registers are **otherwise memory-resident**: `GuestState.gpr[16]`, a flat `uint64_t[16]`. Any
guest register not among the 5 allocated is `ldr`/`str`'d to `[x8 + 8*reg]` on **every** use. `rsp` and
`rip` are *never* allocated — they are always memory (`kRipOffset`; stack ops go through the SoftTlb).

XMM registers have **no host registers at all**: every SSE operation is `ldr q` / `str q` against
`GuestState.xmm` (`xmm_offset()`, `:1486`), so an inline SSE op costs 2–3 memory accesses plus the
vector op.

---

## 5. Is there register allocation? Algorithm and biggest cost.

**Yes, but only a trivial one, and only within a single block** (`:2163-2192`):

```
for each guest reg i: refs[i] = count of read + write mentions in the block
order = regs with refs[i] > 2, sorted by descending refs
allocate up to 5 of them to x19..x23
```

* No liveness analysis. A register mentioned 3+ times gets a host register whether or not it is live
  across the block.
* No live ranges, so no reuse of a host register by two different guest registers in disjoint parts of
  the block.
* No spilling: registers 6..16 simply stay in memory.
* **Allocation never crosses a block boundary.** At every block entry the allocated registers are
  `ldr`'d (`reload_ra()`), at every block exit `str`'d (`spill_ra()`), plus a 128-byte frame and the
  `x19..x28` save/restore in the entry trampoline.

**Biggest cost caused by this: 10 memory operations of pure overhead per block transition.** With
in-JIT chaining, block transitions are frequent (every taken branch that is not a fused self-loop), so
a hot loop's inner block pays 5 loads + 5 stores of register shuffle *plus* 2–4 instructions to
materialise the next PC *plus* ~10 instructions of dispatcher, on top of ~8 instructions per guest
memory access. Measured effect: the "wide" benchmark (11 guest registers, of which only 5 can be
allocated) takes 2.07 ns/guest-insn vs 3.84 ns/guest-insn for the narrow one under qemu — the wide
case is *faster per instruction* only because it has 40 ALU ops per branch to amortise the overhead
over. The overhead is real; the workload happens to hide it.

A second, easily-fixable cost: `spill_ra()`/`reload_ra()` are also emitted around **every helper call**
(`:3013-3020`), but `x19..x23` are callee-saved under AAPCS64, so a C++ callee cannot clobber them.
That spill/reload pair is **provably unnecessary** — pure waste on every untranslated instruction.

---

## 6. How are x86-64 RFLAGS handled?

**Eagerly materialised into six separate bytes in memory, with dead-flag elimination and local NZCV
fusion.** Not lazy, not packed.

* `GuestState::Flags` is six `bool`s (`guest_state.hpp`), i.e. six bytes at fixed offsets.
* A flag-writing instruction emits `cset` + `strb` **per live flag** (`emit_flags_from_nzcv`, `:283-320`),
  and parity costs five extra instructions (`emit_parity`, `:322-328`).
* So a `cmp` with all six flags live is up to **~16 host instructions and 6 separate byte stores**.
* The two mitigations:
  * **Dead-flag elimination** (`:2194-2203`): flags not read before being overwritten are not emitted.
  * **`try_nzcv_cond` fusion** (`:3294-3310`): when the block-ending `Jcc` reads exactly the flags the
    preceding ALU wrote, the flags are *never* materialised — the branch becomes `ADDS/SUBS` + `csel`
    + `br`. This is the single most valuable optimization in the translator and it is
    **implemented + tested** (`translator.dead_flags_are_not_computed`,
    `translator.rmw_memory_alu_followed_by_jcc_preserves_flags`,
    `translator.alu_immediate_is_folded`, all AArch64-gated).
* When fusion does *not* apply, the branch reloads the flag bytes and rebuilds the condition in a
  register (`emit_eval_cond_to_reg`, `:335-395`) — up to 4 loads + 3 ALU ops for `jg`.

Consequence: any non-fused flag consumer is expensive, and the flag bytes are a **store-to-load
dependency chain through memory** that the CPU cannot keep in registers. This is the second-largest
per-instruction cost after guest memory access.

---

## 7. How are basic blocks discovered and terminated?

`translate_cached_block` (`:2119-2161`): start at the fault-in PC, decode forward, append until:

| Terminator | `BlockExit` | Block target |
| --- | --- | --- |
| `Hlt` | `kHalted` | — |
| `Syscall` | `kSyscall` | `insn.address + length` |
| `Jmp` / `Call` / `Ret` | `kJmp` | immediate operand, else 0 |
| `Jcc` | `kJcc` | `insn.address + length` (fall-through) |
| 256 instructions | `kFallThrough` | next PC |
| a decode failure mid-block | `kFallThrough` | next PC (silently truncates the block) |

Notes:

* Blocks are **keyed by exact guest PC**, and a jump into the middle of an existing block simply
  creates a second, overlapping block. That is why the design is correct-by-construction without any
  block splitting or invalidation-on-entry bookkeeping — at the cost of duplicated code.
* `kMaxCachedBlockInsns = 256`, and the fetch window is up to 4096 bytes (`kMaxWindow`).
* Blocks are never re-formed: there is no re-translation, no re-optimisation, no block growth. Once
  translated, a block is immutable until the whole cache is invalidated.

---

## 8. How are branches handled?

All five forms share one mechanism: compute the next guest PC into `x9`, then `br x24` (the in-JIT
dispatcher). None of them patch anything.

| Form | Emitted code (`:3183-3345`) |
| --- | --- |
| **JMP rel/ind** | materialise target (`mov_imm64`, 2–4 insns) or load from GPR/memory → `spill_ra()` → `br x24` |
| **Jcc** | `spill_ra()`; if fused: `csel` on hardware `NZCV`; else `ldrb` flags + rebuild cond + `cmp`; then `csel` picked vs fall-through PC → `br x24` |
| **CALL rel/ind** | evaluate target → save to `[sp+104]` → `rsp -= 8` → **write return address through the SoftTlb** (8-instruction inline TLB write) → `rsp` updated → `br x24` |
| **RET** | `czb rsp` → halt; **read return address through the SoftTlb**; `rsp += 8+imm16`; `br x24` |
| **indirect** | load target from GPR or memory (`emit_inline_tlb_read`, 8 insns) → `br x24` |

Notable: `call`/`ret` push/pop the return address **in guest memory through the SoftTlb**, i.e. every
call is a guest memory *write* (8 instructions) and every return a guest memory *read* (8
instructions). A native `call` is one instruction. That is why function-call-heavy code is
disproportionately slow in Tier 1 — and why a shadow stack would be the single biggest win for such
workloads (see §L).

---

## 9. Block chaining — or does every block return through a dispatcher?

**Chained, but through a runtime hash probe, not through direct linking.** This is the most
interesting part of the design and the one most at odds with its documentation's spirit.

The exit does `br x24`, where `x24` is a shared stub (`build_jit_trampolines`, `:2054-2073`):

```
str   x9, [x8 + rip]        ; commit the next guest PC
lsr   x10, x9, #12
eor   x10, x10, x9
ubfx  x10, x10, #0, #12     ; slot = (pc ^ pc>>12) & 4095
add   x13, x28, #6, lsl #12 ; &ctx->jump_table[0]   (+0x6000)
add   x13, x13, x10, lsl #4 ; + 16*slot
ldp   x14, x15, [x13]       ; {guest_pc, body_entry}
cmp   x14, x9
b.ne  miss
br    x15                   ; direct jump into the next translated block
miss: x0 = kOk; br ctx->jit_exit_epilogue   ; back to C++
```

So a block transition costs **~10 dispatcher instructions + 5 spill stores + 5 reload loads + 2–4 to
materialise the PC**, i.e. **~20–24 host instructions of pure overhead per block**, but *zero* return
to C++. That is genuinely better than a "return to the interpreter loop" design and it is
**implemented + tested** (`translation_cache.direct_map_collision_falls_back_to_map`,
`translation_cache.caches_blocks_and_agrees_with_interpreter`).

What it is **not**: direct linking. A direct-linking JIT would patch the exit into an unconditional
`b <target block>` and only revert on invalidation, making the transition ~1 instruction. Here the
hash lookup happens on *every* transition, forever. The entry stays indirect because the jump table is
the invalidation mechanism.

---

## 10. Translation cache: keys, lifetime, invalidation, synchronisation, eviction

`TranslationCache` (`include/hyperion/translation_cache.hpp`, `src/core/translation_cache.cpp`).

* **Source of truth:** `std::unordered_map<uint64_t, std::unique_ptr<CachedBlock>> blocks_`, keyed by
  exact guest PC. 
* **Acceleration layer:** `direct_[4096]` direct-mapped array keyed by `(pc ^ pc>>12) & 4095`
  (`:65-80`), and the in-JIT `jump_table[4096]` inside `ExecContext` at `+0x6000` with the same hash.
  Both are *caches of the map*, validated by storing the full `guest_pc` alongside the entry, so a
  collision degrades to the map, never to a wrong block. **implemented + tested**
  (`translation_cache.direct_map_collision_falls_back_to_map`).
* **Lifetime:** from first execution to `invalidate_all()`. `CachedBlock` objects are stable pointers
  (`unique_ptr` in a map ⇒ address-stable), which is what makes caching raw `entry` pointers in
  `direct_`/`jump_table` safe across map rehash.
* **Invalidation:** whole-cache only — `invalidate_all()` clears the map, zeroes `direct_`, resets the
  arena and **drops every translation**. Triggered by:
  (a) `code_epoch` change observed at `run()` entry or after a syscall (`:236-244`);
  (b) `fork(2)` in the parent, which bumps the epoch (`process.cpp:556`).
  There is **no per-block invalidation and no page-level invalidation.**
* **Synchronisation:** none, and none needed — a guest process is single-threaded (§23) and each host
  fork gets its own `TranslationCache` (`process.cpp:437`).
* **Eviction:** **none.** The arena only grows (`ExecArena` appends slabs); `blocks_` never shrinks.
  A guest that executes a very large amount of distinct code grows the cache without bound until
  `invalidate_all()`. For long-lived processes this is an unbounded memory leak by design.

---

## 11. Hot-block detection / execution profiling?

**None. Not partial — absent.** There is no execution counter, no threshold, no promotion, no
re-translation of a hot block with better code, no OSR, no interpreter↔JIT migration.

* `dispatch_hits_` / `dispatch_misses_` are **statistics only** (`:65-80`); nothing reads them to make
  a decision, and the CLI never prints them.
* Every block is translated on **first** execution, at full cost, forever.
* The tier is chosen once, by a CLI flag.

Measurable consequence — this is the clearest demonstration that tiering is missing:

```
$ hyperion run --tier=0 --quiet /tmp/tiny    # a static glibc "puts(\"hi\")": 1019 blocks
tier 0: 149 ms/run
$ hyperion run --tier=1 --quiet /tmp/tiny
tier 1: 289 ms/run        # 1019 blocks translated, ~2x SLOWER end to end
$ hyperion run --tier=1 /tmp/sieve 200000    # 3.39M guest insns, 1210 blocks
0.484 s  (vs 1.168 s at tier 0 — 2.4x faster)
```

So for a *short* program the JIT is a net loss, and the runtime has no mechanism to notice. (Caveat:
these are qemu numbers, where translation is far more expensive than on hardware; the direction of the
effect is real, the magnitude is not transferable.)

---

## 12. Self-modifying guest code

Mechanism: a **page-granular code epoch**.

* `GuestAddressSpace::write` bumps `code_epoch_` when writing a page that has `PROT_EXEC`
  (`address_space.cpp:584-588`).
* Writable-and-executable pages are **excluded from the fast write path**
  (`can_write_fast = writable && !executable`, `:612`), so writes to code always take the slow path
  and always bump the epoch.
* `TranslationCache::run` compares the epoch at entry and after each syscall; on change it calls
  `invalidate_all()` — a full flush.

**Correctness gap (real, narrow):** the epoch is checked **only at `run()` entry and after a syscall**.
The in-JIT dispatcher does not check it. A guest that writes to its own executable memory and then
continues without a syscall (no `mprotect`, no `write`) keeps executing stale translations until the
next syscall. In practice almost all SMC goes through `mprotect` (a syscall), which does trigger the
flush — so this is a latent bug rather than an observable one, but it is not safe by construction.
Tag: **implemented, weak.**

---

## 13. Guest memory access translation and validation

Two-level: a **512-entry direct-mapped SoftTlb**, separately for reads and writes, holding
`{page_tag, host_addend}` with the invariant `host = guest + host_addend`
(`include/hyperion/interpreter.hpp:31-67`).

Inline AArch64 fast path, **8 instructions per access** (`emit_inline_tlb_read`, `:1962-1973`):

```
ubfx  x12, addr, #12, #9            ; slot
add   x13, x28, #1, lsl #12         ; &tlb.read_entries
add   x13, x13, x12, lsl #4
ldp   x14, x15, [x13]               ; tag, addend
sub   x14, addr, x14                ; page offset
cmp   x14, #(4096-bytes)
b.hi  slow                          ; miss, cross-page, or unmapped
ldr   out, [addr, x15]              ; the actual load
```

Misses go to an out-of-line stub that calls `soft_tlb_read_slow` → `fast_mem_read`, which validates
against the region table (`translate_page`) and only then populates the TLB. So a guest pointer is
**never dereferenced without validation** — the design promise in `address_space.hpp` holds.
Permissions are enforced at TLB-fill time per direction (`can_read`, `can_write_fast`).
**implemented + tested** (`test_address_space.cpp`, 320 lines).

Cost: **8 host instructions per guest load/store** versus 1 natively, plus a 512-entry TLB that
aliases badly for guests touching more than 2 MiB of hot data. This is the largest single cost in
Tier 1's inner loops.

---

## 14. Faults / signals from generated code

**There is no host `SIGSEGV` handler for guest memory, and none is needed** — because the generated
code never dereferences an unvalidated address. A bad access fails the TLB check, takes the slow path,
fails validation, and returns `false`, which becomes `BlockResult::kFault` via the fault stub
(`:3401-3410`), which restores `rip` and the budget and returns to C++. The run then stops with
`RunStatus::kFault` and the message *"guest memory or arithmetic fault"*.

**Consequence (a real limitation):** the guest can never *observe* a `SIGSEGV`. A guest with a signal
handler for a null-pointer dereference gets its whole run aborted instead. Combined with §23 (no signal
delivery at all), guest fault handling is absent by design and by necessity.

The only host signals the runtime installs are the `SIGSYS` diagnostics (`sigsys_diagnostics.cpp`).
Nothing here handles `SIGSEGV`, `SIGBUS`, `SIGFPE` or `SIGILL` on behalf of the guest.

---

## 15. Cost of the generated-code ↔ runtime transition

Three distinct transitions, with very different costs:

| Transition | Trigger | Cost |
| --- | --- | --- |
| **Block → block** (in-JIT) | every branch | ~20 host instructions, **no C++** (§9) |
| **Block → C++** | jump-table miss, syscall, fault, budget exhaustion | `ldp` ×6 + `ret` unwind, then map lookup / decode / translate |
| **Block → C++ helper → block** | any of ~15% of opcodes | `spill_ra` (5 str) + 8 arg-setup insns + `blr` + `reload_ra` (5 ldr) ≈ **20 host instructions per guest instruction** |

The third is the expensive one and it is *common*: `ADC/SBB/MUL/DIV/IDIV/SHLD/SHRD/XCHG/CMPXCHG/XADD/
BT*/string ops/CPUID/RDTSC/CLD/FXSAVE` and **73 of 115 SSE operations** have no inline translation.
Every one of them pays a full C++ call. On SIMD-heavy or 128-bit-arithmetic code this makes Tier 1
little better than Tier 0 for those instructions.

As noted in §5, the spill/reload around these calls is **unnecessary** (callee-saved registers).

---

## 16. Cost of a syscall vs a translated instruction

Measured three ways:

| Path | Cost | Source |
| --- | --- | --- |
| `MinimalSyscallHandler::handle()` → static switch | **131 ns** | `hyperion_syscall_bench`, native x86-64 |
| `handle()` → compiled adapter (layer 1) | 235 ns | same (the adapter itself does the work) |
| bare host `getpid` | 126 ns | same |
| **a real guest `getpid` end-to-end, Tier 0** | **744 ns** | 100 000 × `getpid` guest, native |
| a real guest `getpid` end-to-end, AArch64+qemu, both tiers | ~6.2 µs | same guest under qemu |

The dispatch layer itself is **excellent** — a guest syscall costs 5 ns more than a bare host syscall.
But the end-to-end cost is **~5.7× that**, and the gap is not dispatch: it is the **unconditional
16 KiB `SoftTlb::flush()` after every syscall** (`translation_cache.cpp:241`,
`interpreter.cpp:2320`). 1024 entries × 16 bytes at ~30 GB/s ≈ 0.55 µs, which accounts for the
entire difference.

For comparison, one translated guest instruction is ~0.3–3 ns, so **a syscall is roughly 200–2500×the
cost of a translated instruction** — normal for a DBT, but ~600 ns of that is avoidable waste paid on
*every* syscall including `write` and `getpid`, which cannot possibly have changed the address space.

---

## 17. Unsupported x86-64 instruction at decode time

`decode()` returns a `DecodeResult` with an error and the run stops with `RunStatus::kUnsupported`
and a message naming the error plus the PC and the first bytes (`interpreter.cpp:3096-3104`;
`translation_cache.cpp:181-190`). The process exits 1.

**There is no interpretation fallback for an undecodable instruction**, in either tier. A guest using
any opcode the decoder does not model (x87 FPU arithmetic, `MMX`, AVX/AVX2, `SYSRET`, `IRET`, `INT`,
`SYSENTER`, `LGDT`, `MOVBE`, BMI*, …) simply fails to run. This is a hard coverage wall, not a
performance issue, and it is honestly reported rather than silently mis-executed.

The decoder *does* refuse rather than approximate: an unsupported prefix combination, a LOCK on a
non-memory operand, or a malformed ModRM is an error, never a silently-wrong decode. 369 lines of
`test_decoder.cpp` cover this.

---

## 18. Unsupported *operation* inside a translated block

Two cases:

1. **Decodable but not inline-translatable** — the normal case (~15% of opcodes): emit a call to
   `execute_cached_helper` (`:3004-3023`) and continue the same block. Correct, slow (§15).
2. **Genuinely untranslatable** (the emitter returns non-`kNone`) — `translate_at` reports
   `kUnsupported`, and `run()` **terminates the entire run**. There is no re-execution of that block
   by the interpreter, no per-block escape hatch. Tag: **implemented, weak.**

---

## 19. How does execution resume after an interpreter fallback?

It does not, at block granularity — there is no such mechanism. The only "fallback" is the
within-block helper call (§18 case 1), which returns to the next instruction of the same translated
block. A block that cannot be translated is not executed at all.

The only interpreter fallback in the system is the **whole-program** one: an AArch64 host that cannot
map `PROT_EXEC` falls back to Tier 0 before anything runs (`exec_mem.cpp:110-125`,
`main.cpp:164`). Tag: **partial** (whole-program only).

---

## 20. x86-64 coverage: implemented / partial / missing

Ground truth is the `Opcode` enum (`decoder.hpp:37-78`) crossed with the translator's inline switch
(`translator.cpp:2232-3012`) and the helper path.

**Decoded and inline-translated** (31 opcodes): `MOV, MOVZX, MOVSX, LEA, ADD, SUB, CMP, TEST, AND, OR,
XOR, INC, DEC, NOT, NEG, SHL, SHR, SAR, ROL, ROR, IMUL, CDQE, CQO, SETcc, CMOVcc, BSWAP, BSF, BSR,
PUSH, POP, LEAVE, NOP, JMP, Jcc, CALL, RET, HLT, SYSCALL` — with size/source restrictions (the
"classic fast ALU" path covers only 32/64-bit register destinations; 8/16-bit forms fall back).

**Decoded but executed by the C++ helper** (15 opcodes): `ADC, SBB, MUL, DIV, IDIV, SHLD, SHRD, XCHG,
CMPXCHG, XADD, BT, BTS, BTR, BTC, STRINGOP` (MOVS/STOS/CMPS/SCAS with REP), plus `CPUID, RDTSC, CLD/STD`.

**SIMD:** 42 of 115 `SseOp` values inline; the other 73 via helper.

**Missing entirely (decoder refuses):** x87 FPU arithmetic (only `FXSAVE`/`FXRSTOR` state saves),
MMX, AVX/AVX2/AVX-512 (all VEX/EVEX encodings), `SSE3/SSSE3/SSE4.1/4.2` beyond a handful, `POPCNT`
(aside from `BSF/BSR`+`F3`→`TZCNT/LZCNT`), `MOVBE`, BMI1/BMI2, `INT`/`INTO`/`IRET`, `SYSRET`,
`SYSENTER`, segmentation, `PAUSE` (probably decodes as NOP?), privilege instructions, `FSGSBASE`
beyond `arch_prctl`.

Practical reading: **this covers freestanding C compiled with `-O2 -static` and not much else.** It does
not cover anything glibc or any modern compiler emits with default flags on x86-64 (SSE4.2 string ops,
`popcnt`, AVX in vectorised loops). That is consistent with what the test suite actually runs.

---

## 21. SIMD/vector support

**Partial, and architecturally shallow.**

* Guest XMM state lives in `GuestState.xmm[16]` (memory). **No XMM register is ever allocated to a
  host register** — the allocator only knows GPRs (§5).
* Inline code does `ldr q`/`str q` around each operation (`xmm_offset`, `:1486`), so every SSE op is
  2–3 extra memory accesses.
* 42 operations are inline (moves, packed integer add/sub/logical/shuffle/unpack, packed shift-by-imm,
  scalar/packed float add/sub/mul/div/sqrt). 73 go through the helper: comparisons, `PMOVMSKB`,
  awkward shuffles, `PSHUFB`, conversions, `MIN/MAX` variants, `COMISS`/`UCOMISS`, `CMPPS` families.

How AVX would be added: the decoder would need VEX/EVEX prefix support (currently absent), the ISA
abstraction would need a vector register file with real allocation, and the `XmmReg`/`YmmReg` layout in
`GuestState` would have to change (it is `alignas(16) {lo, hi}`, so 256-bit needs a new type). Nothing
in the current design *prevents* it, but nothing prepares for it either — there is no IR, no register
class abstraction, and the SSE inline switch is hand-rolled per opcode. Tag: **partial**, and adding
AVX2 properly is a sizeable, mechanical, isolated piece of work (see §J).

---

## 22. Atomics, memory ordering, fences, LOCK

> **Status after post-audit verification: RETRACTED AS A BUG (see §29).** `LOCK` is
> correct-by-construction here, because guest threads are refused. The finding below is kept
> verbatim as written, with a correction appended at the end of this section.

* The decoder **accepts** the `LOCK` prefix on any memory RMW ALU op and records it in
  `Instruction::lock` (`decoder.cpp:656-666`; field at `decoder.hpp:297`).
* **Nothing ever reads that field.** `grep -rn "insn.lock" src/ include/` → zero hits outside the
  decoder. The interpreter and the translator both execute `LOCK ADD [m], r` as an ordinary
  non-atomic read-modify-write.
* There is **no `DMB`/`DSB`/`ISB` anywhere in generated code**, no barrier on the guest's behalf, and
  no `LDXR/STXR`/`LDADD` usage.
* `XCHG`, `CMPXCHG` and `XADD` are also non-atomic (they go through the C++ helper, which does a plain
  read-then-write on the guest buffer).

Why this is currently invisible: the guest is single-threaded (§23) and guest memory is host *heap*
memory, so there is no other thread that can observe a torn value. It becomes a real bug the moment
guest threads exist, or if a guest's code depends on `LOCK` for ordering against another process
sharing memory (which Hyperion cannot provide anyway).

Tag: **partial/incorrect** — the prefix is validated and then silently dropped. That is worse than
refusing it, because a refusing implementation fails loudly. This should either be refused
(`-ENOSYS`/decode error) or actually implemented; today it does neither.

### Correction (post-audit, §29)

The tag above is wrong for this implementation, and the "silent wrong-answer path" framing does not
hold. `LOCK` is a *single-threaded* memory-ordering and atomicity primitive; it only has observable
meaning when another agent (guest thread, or another process mapping the same memory) can access the
same bytes concurrently. Hyperion refuses all of those:

* `clone`/`clone3` with `CLONE_THREAD`, `CLONE_VM` or `CLONE_SIGHAND` returns `-ENOSYS`
  (`process.cpp:371-375`) — there are **no guest threads**, so no second guest agent exists.
* Guest memory is host **heap**, private to the process; Hyperion offers no shared-memory mapping
  that another process could observe (§13). No second process can see a torn value either.

With exactly one agent, an `LOCK`-prefixed RMW and a plain RMW are *indistinguishable*: there is no
interleaving to exclude and no observer for the ordering. Dropping the prefix is therefore not a
wrong answer here — it is the correct execution of a no-op constraint. The invariant that makes the
field safe to ignore is now written down where the field is defined (`decoder.hpp`,
`Instruction::lock`) and pinned by a regression test so that it cannot rot silently:

* `tests/unit/test_decoder.cpp::decoder.lock_prefix_is_recorded_and_does_not_change_semantics` —
  asserts the prefix is recorded, that the decoded instruction is otherwise identical to the
  unprefixed form, and that an invalid `LOCK` target (`F0 48 01 D8`, `lock add rax,rbx`) is rejected.

The residual truth in the original finding is narrower and is *not* fixed by documentation: if guest
threads are ever implemented (plan item 13), `Instruction::lock` must become a real `LDXR`/`STXR` or
`LDADD` sequence, and the invariant recorded in `decoder.hpp` must be revisited in the same change.
That dependency is stated at the field definition, which is where a future implementer will look.

---

## 23. Threads, clone/fork, signals, child processes

* **Threads: refused.** `CLONE_THREAD`, `CLONE_VM`, `CLONE_SIGHAND`, `CLONE_FS`, `CLONE_FILES` and
  friends are in `kUnsupportedCloneFlags` and return `-ENOSYS` (`process.cpp:371-375`, `:604-606`).
  Deliberate and documented: sharing an address space between two guest threads is impossible when
  the translator owns the memory model.
* **fork/vfork/clone(process): implemented + tested.** A guest fork is a **real host `fork()`**
  (`process.cpp:508`), after which the child **deep-copies** the whole `GuestAddressSpace`
  (`process.cpp:407`, copy ctor at `address_space.hpp:85-90`), builds a fresh
  `MinimalSyscallHandler` and a **fresh `TranslationCache`** (`:437`) — so the child re-translates
  everything from scratch. In the parent, the epoch is bumped (`:556`) so the **parent also discards
  its entire translation cache**. Both sides pay a cold start on every fork. For a shell running many
  small commands, translation cost is paid over and over.
* **Signals: recorded, never delivered.** `rt_sigaction` stores the handler and reads it back
  correctly (`syscall.cpp:2042-2095`) and `sigaltstack` is recorded, but there is **no delivery
  path**: no `rt_sigreturn`, no handler invocation, no `siginfo`/`ucontext` construction, and no
  mechanism to interrupt a running block. The code says so explicitly at `syscall.cpp:2045-2050`.
  `ARCHITECTURE.md` claims "guest signal handler invocation"; the code has none. Tag:
  **documented only.**
* **Consequence:** `SIGALRM`-based timers, `SIGCHLD`-driven reaping, `SIGSEGV` handlers, and any
  program that relies on asynchronous signals will not behave as on Linux. `wait4` reaping works
  because Hyperion tracks children itself.

---

## 24. ABI correctness of the generated AArch64

Checked against AAPCS64, item by item:

| Requirement | Status |
| --- | --- |
| **Stack alignment** | ✅ entry trampoline `stp x29,x30,[sp,#-128]!` — 128 is a multiple of 16, so `sp` stays 16-aligned; scratch slots at `[sp+96..127]` fit exactly inside the frame with no overlap against the saved `x19..x28` at `[sp+16..95]` (`:2037-2050`, `:1474-1477`) |
| **Callee-saved registers** | ✅ `x19..x28` all saved/restored by the entry trampoline and exit epilogue (`:2038-2042`, `:2089-2094`); the allocator only uses `x19..x23`, which are callee-saved |
| **LR (`x30`) handling** | ✅ blocks never use `ret` for chaining — they use `br x24`; `blr` is used only for helper calls, and `x30` is dead at that point because the exit epilogue restores it from the frame. Correct, if subtle. |
| **Argument registers for helper calls** | ✅ `x0 = ctx`, `x1 = state`, `x2 = &insn` set explicitly before each `blr` (`:3013-3020`) |
| **Instruction-cache synchronisation** | ✅ `__builtin___clear_cache` after every code write (`exec_mem.cpp:99`, `:186`), which GCC lowers to `dc cvau` / `dsb ish` / `ic ivau` / `dsb ish` / `isb` on AArch64. |
| **Redundant saves** | ⚠️ `spill_ra()`/`reload_ra()` around helper calls are unnecessary (callee-saved), pure waste (§5) |
| **Encoder range checks** | ✅ `B`, `B.cond`, `TBZ`, `ADR` all range-checked and `throw std::out_of_range` (`aarch64_encoder.cpp:689-750`) |
| **Exception safety** | ❌ **no `try`/`catch` anywhere in `main.cpp` or `translation_cache.cpp`.** An encoder range error, an `ExecArena` `mmap`/`mprotect` failure, or `bad_alloc` propagates out of `run()` and calls `std::terminate` — an abort with no diagnostic. On Android, where `mprotect(RX)` can legitimately fail, `ExecMem::executable_supported()` guards the common case, but any *later* allocation failure is an abort. |

**Verdict: ABI-correct**, with one real robustness hole (uncaught exceptions) and one small
inefficiency (unnecessary spills).

---

## 25. Executable memory on Android/Termux

`ExecArena` / `ExecMem` (`src/core/exec_mem.cpp`):

* Slabs are `mmap(PROT_READ|PROT_EXEC)` up front, with a documented fallback to `RW` + `mprotect(RX)`
  for policies that refuse direct `PROT_EXEC` on anonymous mappings (`:141-156`).
* **Strict W^X per allocation**: every `allocate()` does `mprotect(page, RW)` → `memcpy` →
  `mprotect(page, RX)` → `__builtin___clear_cache` (`:170-190`).
* Capability probe: `executable_supported()` maps one page and tries `mprotect(RX)`, cached in a
  `static` (`:110-125`) — exactly the right check for an Android SELinux domain that denies exec.
  **implemented + tested.**

**Safety: good. Efficiency: poor, and it is the dominant translation cost.**

* **Two `mprotect()` syscalls per translated block.** Blocks are allocated back-to-back in the same
  page, so the *same page* is toggled RW→RX for every single block placed in it. Each `mprotect` is a
  syscall plus a TLB shootdown of the affected range; the `clear_cache` adds a `dc`/`ic` sweep.
  Translating 1019 blocks (the tiny guest) therefore issues ~2038 `mprotect` calls plus 2038 cache
  flushes before the guest does anything.
* The `ExecArena` never reclaims; a long-running guest that invalidates (any `fork`) discards the
  arena (`reset()`), so the whole code cache is re-`mmap`ed and re-toggled.
* No dual mapping (`RW` alias + `RX` alias of the same pages), which is the standard way to write code
  without toggling protection, and no `memfd`-backed executable mapping (which some Android versions
  require). Those are the two mechanisms a production Android JIT would use.

This is the clearest instance of "implemented, weak": correct W^X, but paid per block instead of per
slab.

---

## 26. Every unnecessary source of overhead I could find in the hot path

Ordered by my estimate of impact. Each is a concrete, localised change candidate — listed here, not
implemented.

| # | Overhead | Location | Impact |
| --- | --- | --- | --- |
| 1 | ~~**Unconditional 16 KiB SoftTlb flush after every syscall**~~ — **FIXED, see §29** | was `translation_cache.cpp:241`, `interpreter.cpp:2320` | measured 725 → ~390 ns per guest `getpid` (1.85×) after the fix |
| 2 | ~~**Two `mprotect` + `clear_cache` per translated block**~~ — **FIXED, see §29.6** | was `exec_mem.cpp:170-190` | was 99.8 % of publish cost (2.787 µs/block); now one publish per write window, 0.31 mprotect calls per block measured |
| 3 | **Register spills on every block transition** (5 stores + 5 loads) | `spill_ra`/`reload_ra` | ~10 memory ops per block, forever (no cross-block allocation) |
| 4 | ~~**Redundant spill/reload around helper calls**~~ — **RETRACTED, see §29: these stores are required** | `:3013-3020` | not overhead: `execute_cached_helper` re-reads `GuestState`, so the spills are the state sync point |
| 5 | ~~**No direct linking** — hash lookup on every block exit~~ — **FIXED, see §29.7** | was `:2054-2073` | now one `b` for a statically known successor; 19 % off a branch-heavy guest under qemu |
| 6 | **Flags materialised to 6 separate bytes** with `cset`+`strb` each | `:283-320` | up to 16 instructions for a non-fused `cmp` |
| 7 | **8-instruction SoftTlb fast path** per guest memory access | `:1962-1973` | 8× a native load |
| 8 | **512-entry TLB** (2 MiB coverage) | `interpreter.hpp:38-40` | aliases on real workloads; misses cost a C++ call |
| 9 | **`call`/`ret` push/pop through the SoftTlb** | `:3255-3300` | 8+8 instructions per call/ret pair; a shadow stack would remove both |
| 10 | **Tier 0 virtual call `code_epoch()` per instruction** | `interpreter.cpp:3076` | prevents inlining, ~5 cycles |
| 11 | **Tier 0 eager 6-flag computation** with `__builtin_parity` | `alu::adc` | ~10-20 cycles per ALU op |
| 12 | **Tier 0 per-access function call + `memcpy`** | `fast_mem_read` | ~15-25 cycles per access |
| 13 | **`mov_imm64` (2–4 insns) to materialise the next PC on every block exit** | `:3204` etc. | could be an `adrp`+`ldr` from a per-block constant, or a precomputed delta |
| 14 | **No block reuse of decoded operands**: `emit_cached_body_insn` re-reads `Instruction` fields per emit | throughout | translation-time only, minor |
| 15 | **No eviction** → unbounded arena/map growth | `translation_cache.hpp` | memory, not speed |
| 16 | **Child re-translates everything after fork; parent discards its cache** | `process.cpp:437,556` | fork-heavy guests pay cold start repeatedly |
| 17 | **XMM always memory-resident** | `xmm_offset` | 2–3 extra accesses per SSE op |
| 18 | **No hot/cold separation, no tiering** | absent | short programs pay full JIT cost for no benefit (§11) |

No locks, no allocations and no host/guest state copies exist on the per-instruction path, which is
to the design's credit. `lookup()` is allocation-free; the map is only touched on a miss.

---

## 27. What the benchmarks actually measure, and what they miss

**`hyperion_tier_bench`** (`benchmarks/tier_bench.cpp`) — two fixed synthetic guest programs (a
3-instruction accumulate loop, and an 11-register wide variant), run through both tiers, reporting
ns/guest-instruction with median + IQR, one discarded warm-up, a checksum, and an explicit
plausibility floor. Honest about being unavailable on non-AArch64 (`tier 1: unavailable`, verified
above). **What it does not do:** it does not use a real ELF, does not include the syscall path, does
not measure translation cost, does not measure memory, uses `FlatMemory` (so it bypasses the region
table and the address-space validation), and its two workloads are both "no memory access, no call, no
ret, no SIMD" — the three things that dominate real code.

**`hyperion_syscall_bench`** — measures `handle()` dispatch layers against a bare host syscall. Good
and honest (it states it is host-CPU-only). **But it measures *only* the dispatch call**, so it
completely misses the 16 KiB SoftTlb flush that a *real* guest syscall pays (§16) — which is why the
bench says 131 ns and a real guest pays 744 ns.

**`hyperion_bench`** — host calibration only, not DBT.

**`tests/differential`** — 11 cases comparing generated/hardware x86-64 execution against the
interpreter; on AArch64 all 11 skip because there is no x86-64 oracle. So on the *only* host where
Tier 1 runs, the hardware differential oracle is unavailable; Tier 1's correctness rests on the
AArch64-gated translator differentials instead.

**Important DBT characteristics with no benchmark at all:**

1. **Translation throughput and cost** (blocks/s, bytes of AArch64 per guest instruction, time to
   translate the first block). Nothing measures this, yet it decides whether the JIT is worth it (§11).
2. **Cache size / code growth / eviction behaviour** — `total_code_bytes()` exists and is never called
   by any benchmark.
3. **Dispatch hit/miss rates** — `dispatch_hits_`/`dispatch_misses_` are collected and never reported.
4. **Block-transition cost** (the chaining overhead in §9) — unmeasured.
5. **Memory-access cost in isolation** (SoftTlb hit vs. miss) — unmeasured, despite being the largest
   inner-loop cost.
6. **Realistic workloads**: every measured guest is either a synthetic loop or a hand-written kernel.
   `benchmarks/workloads/{sieve,sha256,quicksort_c,qsort_glibc,nqueens}.c` exist but are not wired
   into `tier_bench`; I ran them manually (§3, §11) to get end-to-end numbers.
7. **Syscall-heavy workloads** (the flush bug in §16 would have been found immediately by one).
8. **Cold-start / `--tier` crossover point** (which is exactly the `tiny` result above).
9. Nothing measures **real hardware**. `BENCHMARKS.md` reports figures like *"393.3 MIPS"* and
   *"`104.49×`"* for Tier 1 — those are numbers for **running Hyperion's output under `qemu-aarch64`
   on x86-64**, and they should not be read as achievable Hyperion performance. The tables are labelled
   `qemu-aarch64`, which is honest, but "MIPS" invites exactly the wrong reading.

---

## 28. Can this realistically evolve into a high-performance production DBT?

**Yes — the foundation is sound; the optimiser and the runtime services are absent.** No rewrite is
required for the *translation* side. The parts that would need redesign are the parts that assume a
single-threaded, single-address-space, whole-program process.

What is genuinely good and worth keeping:

* Decoded `Instruction` → AArch64 directly, with a real encoder and a real label/fixup mechanism.
* Correct-by-construction block keying by exact PC (overlapping blocks, no splicing bugs).
* SoftTlb with a proven no-false-hit invariant, and validation *before* dereference — the security
  posture is right.
* In-JIT chaining, so block transitions never return to C++.
* Self-loop superblocks and NZCV fusion — the two optimizations present are the right two.
* Honest failing: unsupported instructions abort the run rather than mis-executing, and the 24
  AArch64-gated tests genuinely execute generated code and diff it against the interpreter.

What blocks "high performance" today:

1. Per-block `mprotect` (§25) — caps translation rate.
2. No cross-block register allocation (§5) — caps generated-code quality on branch-dense code.
3. Flags in memory (§6) — caps ALU throughput.
4. No direct linking (§9) — caps branch throughput.
5. 15% of opcodes interpreted in-block (§15) — caps SIMD/128-bit-heavy code.
6. No tiering (§11) — makes the JIT a net loss on short processes, which is most CLI usage.
7. Per-syscall TLB flush (§16) — caps syscall-heavy workloads.

None of those is architectural; all seven are localised. The architectural blockers are elsewhere:
guest threads (§23), signal delivery (§23), and the whole-process fork model — those *are* rework.

---

# A. Current DBT architecture

```
                       hyperion run <x86-64 ELF>
                                  |
                     src/cli/main.cpp:324 cmd_run
                                  |
                 :205 run_elf -> GuestAddressSpace + GuestState
                    elf_loader.cpp: load_linux_elf_process
                      (PT_LOAD, PT_INTERP, System V stack/auxv)
                                  |
             +--------------------+---------------------+
             |                                          |
   tier 0 (default on x86-64)              tier 1 (default on AArch64)
   interpreter.cpp:3057                    translation_cache.cpp:158
             |                                          |
   per-instruction:                        per-block: fetch window (4096 B)
     epoch check                             -> translator.cpp:2102
     decode cache (8192)                       decode/copy
     fast_mem_read/write (SoftTlb)             reg alloc (5 regs, x19..x23)
     eager 6-flag ALU                          flag liveness
             |                                   emit AArch64 -> ExecArena::allocate
             |                                     (2x mprotect + clear_cache)
             |                                          |
             |                                   enter_jit trampoline (x19..x28 saved)
             |                                          |
             |                                   block body -> br x24 (in-JIT dispatch)
             |                                          |
             +----------------> syscalls ------> MinimalSyscallHandler::handle
                                  (4-layer dispatch, adapters)
                                       |
                                  TLB flush (16 KiB) + epoch check
                                       |
                                  exit / fault / step limit -> RunResult
```

Memory model: guest addresses are **abstract**; backing bytes are host heap (`Region::bytes`); a
512-entry-per-direction SoftTlb caches `{page_tag, host_addend}`; permissions are checked at fill time.

# B. Exact execution pipeline for `hyperion run <elf>`

1. `cmd_run` parses `--tier/--max-steps/--sysroot/--quiet/--dump`, splits Hyperion options from guest
   argv, reads the file.
2. `looks_like_elf` → `run_elf` (`main.cpp:205`).
3. `GuestAddressSpace` created: 256 MiB heap at `0x20000000`, 1 GiB mmap ceiling at `0x7f0000000000`.
4. `load_linux_elf_process`: PT_LOAD segments mapped with per-segment permissions, `PT_INTERP` resolved
   through `--sysroot`, `argc/argv/envp/auxv` pushed onto a System V stack, `AT_EXECFN`/`/proc/self/exe`
   set to the guest image.
5. `MinimalSyscallHandler` constructed; `enable_linux_compat(sysroot)`; guest image path recorded;
   JSON registry loaded **once** (never re-read during execution).
6. If `tier == 1 && !ExecMem::executable_supported()` → warn, `tier = 0`.
7. `run_program(...)` (Tier 0) or `TranslationCache::run(...)` (Tier 1).
8. Tier 1 loop: `translate_at(pc)` → `lookup` (direct-mapped → map → translate) → `enter_jit` →
   in-JIT chained execution → return on syscall/fault/halt/budget → handle syscall → flush TLB,
   maybe invalidate → repeat.
9. `RunResult` → exit status is the guest's own (`result.exit_code & 0xFF`) when it exited via
   `exit_group`.

# C. Tier 0 analysis

Interpreter with a decoded-instruction cache. **implemented + tested.** 63 MIPS on this host
(15.85 ns/instr); the gap to a good interpreter is items 1, 4, 5 and 7 of §2, plus the
per-syscall flush. It is the *only* reachable tier on x86-64 and it is the correctness oracle for
Tier 1. It is also the thing that makes "hyperion runs x86-64 binaries" true on any Linux host
regardless of exec permissions — which is genuinely valuable.

# D. Tier 1 analysis

Baseline block JIT, direct-to-AArch64, no IR. **implemented + tested on AArch64 only.**
Real optimizations: dead-flag elimination, NZCV fusion, self-loop superblocks, block-local register
allocation. Real omissions: no cross-block allocation, no direct linking, no tiering, no eviction,
no SMC during-block, 15% of opcodes via C++ helper, no atomics, XMM never allocated. Unreachable on
x86-64; ~2.4× faster than Tier 0 on a real program and ~91–183× on a synthetic ALU loop, all under
qemu (so *relative only*). For a short program it is 2× **slower** end to end.

# E. Translation/cache architecture

Map + two 4096-entry direct-mapped accelerators (one in C++, one in `ExecContext` at `+0x6000`,
shared with the generated code). Keyed by exact PC, so overlapping blocks are correct by construction.
Whole-cache invalidation only, triggered by code-epoch change at run entry / after syscalls, and by
fork. No eviction, no per-block or per-page invalidation, no synchronisation (single-threaded).
Code lives in `ExecArena` slabs with per-allocation W^X toggling.

# F. Current x86-64 coverage

~46 opcodes decoded (of which 31 inline-translated in Tier 1 and 15 via helper); 115 SSE2 ops (42
inline); everything else refused at decode. Adequate for `-O2 -static` freestanding C; inadequate for
glibc-built programs with default flags, for x87, for AVX, and for anything using atomics meaningfully.

# G. Current performance bottlenecks

In order: (1) per-block `mprotect` on translation; (2) 8-instruction SoftTlb path per memory access;
(3) flags materialised to memory; (4) register spill/reload per block transition; (5) hash-probe
dispatch per transition instead of direct linking; (6) `call`/`ret` through the SoftTlb; (7) 16 KiB
TLB flush per syscall; (8) 15% of opcodes via C++ call; (9) no tiering, so short runs pay for nothing.

# H. Current correctness risks

| Risk | Severity | Status |
| --- | --- | --- |
| **`LOCK` prefix decoded then ignored** — non-atomic RMW | High (latent) | §22, no test covers it |
| SMC not observed inside a block without an intervening syscall | Medium (latent) | §12 |
| Guest signals never delivered, though `sigaction` succeeds | High for real programs | §23, documented in code, contradicted by `ARCHITECTURE.md` |
| Guest cannot receive `SIGSEGV`/`SIGFPE`; whole run aborts | Medium | §14 |
| No `try`/`catch` around `run()` — encoder/arena failures `std::terminate` | Medium (Android-relevant) | §24 |
| Unbounded code cache growth | Low | §10 |
| Tier 1 aborts the run on one untranslatable block | Low | §18 |
| Blocks/chaining/dispatch verified only on AArch64; 24 tests skip on x86-64 | Medium | §27 |

Genuinely *not* risky, contrary to what one might expect: ABI/frame layout, i-cache coherency, guest
pointer validation, and refusing undecodable instructions. Those are done properly.

# I. Missing for a serious high-performance DBT

Correctness-completing: atomics/`LOCK`, signal delivery (`rt_sigreturn`, handler frames,
`siginfo`), guest `SIGSEGV`/`SIGFPE` delivery, x87, AVX/AVX2, SSE4.x, `POPCNT`, `MOVBE`;
threads (a real design problem, not a patch).

Performance-completing: direct linking, cross-block register allocation with spilling, flag
laziness (keep NZCV, materialise on demand), shadow stack for `call`/`ret`, profile-guided hot-block
promotion + interpreter fallback, conditional TLB flush, slab-level W^X with dual mapping,
region-table-free fast path (larger SoftTlb, huge pages), inline code for the remaining 15% of opcodes.

Tooling: a real per-block benchmark, translation-cost metrics, hit/miss reporting, and a hardware
target to measure on at all.

# J. Incrementally addable (no redesign)

In rough order of value/effort:

1. **Conditional SoftTlb flush** — flush only when `code_epoch` changed or a mapping syscall ran
   (`mmap/munmap/mprotect/brk/execve`). Removes ~600 ns from every syscall. Small, local, measurable.
2. **Remove redundant spill/reload around helper calls** (§5). Delete two calls. Pure win.
3. **Slab-level W^X**: map a slab `RW`, write N blocks, `mprotect` to `RX` when the slab fills (or on a
   flush), instead of per block. Turns 2 syscalls/block into 2 syscalls/slab.
4. **Direct linking**: patch each exit into `b <target>` when the target is known and stable, fall back
   to the hash dispatch otherwise; un-patch on invalidation. Turns ~20 instructions/transition into ~1.
5. **Hot-block gating**: count executions in a small array; interpret until N, then translate. Fixes the
   "JIT is slower for short runs" problem and gives a natural place for an interpreter fallback.
6. **Larger SoftTlb** and a second-level check to cut misses on real workloads.
7. **Shadow stack for `call`/`ret`** — removes 16 instructions per call/return pair.
8. **Inline the remaining hot helper opcodes** (`ADC`/`SBB`/`MUL`/`DIV` with hardware
   `ADCS`/`SBCS`/`UMULH`/`SDIV`+overflow fixup).
9. **Report the metrics you already collect** (`dispatch_hits_`, `dispatch_misses_`,
   `total_code_bytes()`, block/translation counters) and add a translation-cost benchmark.
10. **XMM allocation** — extend the allocator to a second register class for the 4–8 vector
    temporaries used per block.
11. **`try`/`catch` boundary** around translation and allocation so an Android `PROT_EXEC` denial or an
    allocator failure is a clean error, not `std::terminate`.

# K. Requires architectural change

1. **Guest threads** — needs a shared, refcounted `GuestAddressSpace` shared by N host threads, a
   thread-safe translation cache, per-thread `GuestState`, and a memory model. Today fork deep-copies
   the address space precisely because sharing is impossible.
2. **Signal delivery** — needs exact guest PC at every instruction boundary (currently `rip` is only
   written at block exits, `write_rip=false`), block-boundary safe-points, synthetic stack frames
   (`sigframe`/`ucontext`), and `rt_sigreturn` interception. This is the deepest change and it also
   unblocks guest `SIGSEGV`/`SIGFPE`.
3. **Atomics that mean something** — requires either a shared-memory model (above) or refusing `LOCK`
   honestly. Cheap version: refuse it today. Expensive version: `LDXR/STXR` sequences once threads
   exist.
4. **An IR** — the current design emits AArch64 directly from the decoded instruction, per instruction,
   with no intermediate form. Real optimization (CSE, LICM, scheduling, cross-block allocation,
   regalloc with spilling) needs a CFG-level IR. This is a rewrite of `translator.cpp`'s emission, but
   it does **not** invalidate the decoder, the encoder, the SoftTlb, the trampoline ABI, or the tests.
5. **Process model** — deep-copying the address space per fork and discarding both caches is fine for
   correctness and terrible for `fork`-heavy guests; a CoW page-sharing scheme would need the region
   table to support shared backing.

# L. Prioritized plan, highest impact / lowest risk first

| P | Work | Risk | Why now | Outcome |
| --- | --- | --- | --- | --- |
| 1 | Refuse `LOCK` (or implement it) — **correctness bug, tiny fix** | None | Currently a silent wrong-answer path; §22 | **Not a bug — justified and pinned, §29.1.** Threads are refused, so LOCK ≡ plain RMW; invariant documented in `decoder.hpp`, regression test added. |
| 2 | Conditional SoftTlb flush | Low | ~600 ns × every syscall, measured | **Done, §29.2.** 725 → ~390 ns per guest `getpid`. This exposed a pre-existing latent stale-TLB bug that the unconditional flush had been masking (new `mapping_epoch()`). |
| 3 | Drop redundant spills around helper calls | None | Free | **RETRACTED, §29.3 — do not do it.** The spills are the required `GuestState` write-back before the helper re-reads state. |
| 4 | Metrics: report existing counters + add a translation-cost and syscall-heavy benchmark | None | Everything after this needs a baseline; §27 | **Done, §29.5.** Translation cost is now measured (11 608 ns/guest-instruction under qemu), syscall-heavy guest benchmarked end to end, counters printed by `hyperion run --tier=1`. |
| 5 | Slab-level W^X | Low | Largest translation-time cost | **Done via option A, §29.6** (not the "low risk" version the audit implied): one mprotect per write window instead of two per block, 2.00 → 0.31 calls/block on a real guest, strict W^X kept. Cost: bounded successor prefetch (16 blocks / 3584 bytes per window). |
| 6 | Hot-block gating (interpret → promote) | Medium | Makes Tier 1 a win on short programs; §11 |
| 7 | Direct linking of block exits | Medium | ~20 → ~1 instructions per transition | **Done, §29.7** — piggybacks on P5's write window; 19 % on a branch-heavy guest under qemu, no regression on the self-loop workload. |
| 8 | Cross-block register allocation with spilling | Medium-High | Biggest codegen win; needs liveness over the CFG |
| 9 | Lazy flags (keep `NZCV`, materialise on demand) | Medium | Removes 6 byte-stores per ALU op | **Stage 1 done, §29.11** (PF/AF only): descriptor + three forced points, code size −2.6 %, PF/AF materialisations 30.0 M → 575 on a branch-heavy guest, runtime unchanged. CF/ZF/SF/OF stay eager (stage 2, still open). |
| 10 | Shadow stack for `call`/`ret` | Medium | 16 instructions per call/return |
| 11 | Inline the remaining integer helper opcodes | Low | 15% of opcodes currently pay a C++ call | **Done, §29.8.** Measurement said the 15 % was really 8/16-bit six-flag ALU: 9 012 090 → 257 helper calls (−71 % wall time on a byte-comparing guest, no change on two others), code size −4.5 %. |
| 12 | Signal delivery infrastructure | High | Unlocks real programs; needs `rip` at every instruction |
| 13 | Guest threads | Very High | Architectural; needed for anything concurrent |
| 14 | An IR + real optimizer | Very High | The correct long-term shape; nothing above requires it |

Items 1–5 are each a day's work and independently verifiable with the existing test suite. Items 6–11
are the ones that turn a baseline JIT into a fast one. Items 12–14 are the ones that turn it into a
system.

---

## 29. Post-audit log: what was implemented, what was retracted, what was measured

This section was added *after* the audit, when the P1–P5 items of §L were implemented. Everything
below was verified against source and re-measured on the same host as §0 (**x86-64 Linux for Tier 0
natively; AArch64 only via cross-build under `qemu-aarch64-static`**). Where a claim in §1–§28 turned
out to be wrong, the original text is kept and marked here rather than quietly rewritten.

### 29.1 P1 — `LOCK` is not a bug in this design (finding retracted, behaviour pinned)

`LOCK` is only observable when a second agent can touch the same bytes. Hyperion refuses
`CLONE_THREAD`/`CLONE_VM`/`CLONE_SIGHAND` (`-ENOSYS`, `process.cpp:371-375`) and gives the guest
private host-heap memory, so a single-threaded `LOCK`-RMW is semantically identical to a plain RMW.
Dropping the prefix is correct here, not a silent wrong answer.

What was done instead of "implementing" it:

* The invariant — *`Instruction::lock` may be ignored only while guest threads are refused* — is
  documented at the field (`include/hyperion/decoder.hpp`), together with what must change when
  threads land (a real `LDXR`/`STXR`/`LDADD` sequence, and plan item 13).
* `tests/unit/test_decoder.cpp::decoder.lock_prefix_is_recorded_and_does_not_change_semantics`
  locks this in: the prefix is recorded, the result is identical to the unprefixed decode, and an
  invalid `LOCK` target is still rejected.

Nothing in the execution path was changed for P1, because there was nothing to change.

### 29.2 P2 — conditional SoftTlb flush (done) and the latent bug it exposed

The flush is now conditional. Making it conditional alone **broke the dynamic loader guest**
(`elf_loader.executes_dynamic_pt_interp_elf64_via_ld_linux`, guest fault at `mov` reading `0x8`),
which is the interesting part of this item.

Root cause — a **pre-existing latent bug that the unconditional 16 KiB flush had been masking**:
`code_epoch_` was doing double duty as both the self-modifying-code counter and the
mapping-invalidation counter, but it was only bumped when *executable* memory moved —
`protect()` gated on `exec_changed`, `map_anonymous`/`map_fixed` on `has_perm(perm, kExec)`,
`unmap` on `had_exec`. Any ordinary non-exec mapping change could therefore relocate backing
storage without the TLB noticing. The concrete trigger is a RELRO `mprotect`, which calls
`split_region_at()` (`address_space.cpp:95`) and replaces a region's tail buffer with a fresh
allocation; every cached `host_page` for those pages then points into freed memory. A read-path
probe comparing `memory.translate_page()` against the cached `host_page` printed
`STALE read tlb page=0x8000001ee000 cached=… real=… epoch=43` immediately before the fault.

Fix — two independent generations instead of one:

| Piece | Where |
| --- | --- |
| `Memory::mapping_epoch()` (virtual; default returns `code_epoch()` so any other `Memory` stays conservative) | `include/hyperion/interpreter.hpp` |
| `GuestAddressSpace::mapping_epoch()` + `mapping_epoch_` | `include/hyperion/address_space.hpp` |
| Unconditional bump at the top of all 9 mutators: `reset`, `map_image`, `map_segment`, `map_anonymous`, `map_fixed`, `unmap`, `protect`, `set_brk`, `ensure_heap_mapped` | `src/core/address_space.cpp` |
| Per-instruction `mapping_epoch()` validation in the decode cache; the post-syscall flush deleted | `src/core/interpreter.cpp` |
| `cached_map_epoch_` in the cache; entry flush and post-syscall flush now conditional | `include/hyperion/translation_cache.hpp`, `src/core/translation_cache.cpp` |

Deliberate choice: `code_epoch_` is *not* bumped for non-exec mappings. Bumping it would invalidate
the entire translation cache on every `mmap`, which is a far worse regression than the flush being
conditional. The two counters are kept separate, and the separation is what the new test asserts.

New test `tests/unit/test_address_space.cpp::address_space.every_mapping_mutation_bumps_the_mapping_generation`
covers each mutator (including the non-exec `protect`, the split-triggering `protect`, and both
`set_brk` directions) and asserts the two generations stay independent. It is **not vacuous**: with
the bump commented out in `protect()` alone it fails with
`mapping_epoch did not move for: protect(non-exec)`.

Measured effect (native x86-64, Tier 0, 100 000 guest `getpid` calls):

| | ns per guest syscall |
| --- | --- |
| before (HEAD `7a370a6`) | 725 |
| after | ~390 (1.85×) |

The remaining ~260 ns above the 131.5 ns dispatch floor is per-instruction dispatch, not the flush —
i.e. this item is finished and further syscall gains need a cheaper interpreter loop (§26 rows 10,
12), not a cheaper invalidation.

### 29.3 P3 — retracted, the spills are required

§26 row 4 said the spill/reload pair around a helper call was redundant. It is not.
`execute_cached_helper` (`interpreter.cpp:3012`) calls `execute_decoded(state, memory, tlb, insn,
nullptr)`, i.e. control leaves generated code and re-enters the C++ interpreter, which reads and
writes `GuestState`. The generated code's live registers therefore *must* be written back to
`GuestState` before the call and reloaded after it, or the helper operates on stale state. This is
correct state synchronisation, not dead work. Removing it would be a correctness regression, and no
attempt to do so was made.

### 29.4 Verification state after P1 + P2

| Host | What ran | Result |
| --- | --- | --- |
| x86-64 Linux, native | `build-strict` unit suite | **341 passed / 0 failed / 28 skipped (of 369)** |
| x86-64 Linux, native | `ctest` | 19/19 (two new tests: `translate_bench.smoke`, `translate_bench.rejects_single_sample`) |
| x86-64 Linux, native | dynamic-loader guest (`PT_INTERP` → `ld-linux`, exercises the RELRO `mprotect` path) | OK after the fix |
| AArch64, cross-built, **`qemu-aarch64-static`** (no hardware) | unit suite | **369 passed / 0 failed / 5 skipped (of 374)** |
| AArch64 under qemu | `hyperion_tier_bench` | 436.55 ns/guest-ins Tier 0 vs 3.6895 Tier 1, both correct |
| AArch64 under qemu | sieve, 200 000 | Tier 0 1332 ms vs Tier 1 458 ms |

The 24 native / 5 qemu skips are the same arch-gated set as before (§Appendix), not new gaps.

**Not verified, and not claimed:** native AArch64 timings (no access to such hardware), real
Android/Termux behaviour, 16 KiB-page hosts, and anything requiring guest threads or signals.
Tier 1 remains unreachable on x86-64 for the reason in §0 — every Tier 1 number here is
qemu-emulated and should not be read as a hardware measurement.

The P2 syscall figure above is a **CLI end-to-end** number, which silently includes process
startup (ELF load, registry, arena). Re-measured in-process with one harness linked against both
trees, so that startup is excluded and both sides are otherwise identical:

| tree | ns per guest `getpid` (200 000 calls, best of 3, native x86-64) |
| --- | --- |
| HEAD `7a370a6` | 508 |
| with P2 | **188** (2.7×) |

That is the number to trust for the syscall path itself. The floor is the 131.5 ns
`existing_dispatch` cost from §16; the rest is the four guest instructions per call.

### 29.5 P4 — counters surfaced, translation cost measured

Three of the counters §27 complained about already existed (`blocks`, `total_code_bytes()`,
`dispatch_hits_`/`dispatch_misses_`) and were only printed by `hyperion_tier_bench`. Changes:

* `TranslationCache` now measures its own translation cost: `translation_ns()` (wall time on the
  miss path, via a timer guard so failed translations are counted too) and
  `translated_guest_instructions()`. Measured on the miss path only — nothing was added to the
  per-instruction path.
* `hyperion run --tier=1` prints them (three extra lines after the pre-existing
  `tier 1: N basic block(s) translated` line, which is left untouched because
  `tests/run_cross_tests.cmake` matches it):
  `N guest instruction(s) translated into B byte(s) of generated code`,
  `dispatch H hit(s) / M miss(es)`, and `translation T ms wall (U ns per translated guest instruction)`.
* New benchmark `benchmarks/translate_bench.cpp` (`hyperion_translate_bench`), with two
  registered CTest entries. It answers the two questions a per-instruction benchmark cannot:
  1. **translation cost** — reported from the cache's own counters, so it is measured rather than
     inferred by subtracting a fast run from a slow one;
  2. **guest syscall cost** — a tight `getpid` loop (`mov eax,39; syscall; dec rcx; jnz`), timed
     end to end at tier 0 and tier 1, with `--compat` to cover the handler path `hyperion run`
     uses. The harness asserts the exact retired-instruction count (`4·N + 1`), so a fast number
     from a broken run cannot be published.

Measured, AArch64 **under qemu** (so: emulation, not hardware), 3-block workload, 468 generated
bytes: **11 608 ns per translated guest instruction**, i.e. ~300 µs for a three-block translation
pass, with an IQR fraction of 0.167. On this host that figure is dominated by emulated `mprotect`
(see §29.6), which is exactly why the next item matters and why the number must not be read as an
AArch64 hardware cost.

### 29.6 P5 — measured, and the audit's "low risk" label is wrong

§26 row 2 and plan item 5 say the per-block `mprotect` pair is the largest translation-time cost and
that removing it is low risk. The cost part is right; the risk part is not, because of a structural
reason the audit missed.

Measured cost of exactly what `ExecArena::allocate()` (`exec_mem.cpp:207-216`) does per block —
`mprotect(RW)` + `memcpy(156 B)` + `mprotect(RX)` + `clear_cache` — on this **x86-64 host**
(a host proxy; Tier 1 cannot run natively here):

| per 1210 translated blocks | total | per block |
| --- | --- | --- |
| `mprotect` pair + `memcpy` (current) | 3372 µs | 2.787 µs |
| `memcpy` only | 7.6 µs | 0.006 µs |
| **protection churn share** | **99.8 %** | 2.781 µs |

So the protection changes are not merely the largest part of translation time, they are essentially
*all* of it: about **3.4 ms per 1210-block guest** on this host, spent on nothing but page-table
permission toggling.

The structural problem: a translated block must be **executable immediately** — the dispatcher
enters it the microsecond it is written — while page permissions are per-page and blocks are ~156
bytes. Any scheme that removes the per-block toggle has to either batch writes before publishing or
give up strict W^X:

| option | mechanism | win | cost |
| --- | --- | --- | --- |
| A. speculative successor translation | on a miss, translate the block *and* its branch target/fall-through into one write window, publish once | ~1 `mprotect` per 3 blocks today, per ~page if successors chain (≈6-25×) | translates code that may never run; **changes `block_count()`/`translation_count()`, which tests pin exactly** (`test_translation_cache.cpp:83,121`) |
| B. page-per-block with a rotating write page | leave published pages RX, publish each new block's page once | exactly 2× | wastes up to one page per block (≈4 KiB/block, ~5 MB for a 1210-block guest) |
| C. dual mapping (`memfd_create` + RW alias + RX alias) | write through the RW alias, execute through the RX alias; no `mprotect` per block at all | ~50×+ | the same physical bytes are writable and executable simultaneously — **strictly weaker than the W^X property this project documents** (SECURITY.md, DECISIONS.md D-016) |

None of these is "low risk" in the sense §L claimed: A changes pinned observable behaviour, B
multiplies arena memory by ~25×, C trades away a documented security property.

**Option A was chosen and implemented.** The arena now has a write window:

* `ExecArena::begin_write()` / `publish()` — slabs are mapped writable and non-executable; `publish()`
  flips every page written since the last publish to read-execute and flushes the I-cache. No page is
  ever writable *and* executable, so this is still strict W^X, not W^X-by-alias (option C was rejected
  on exactly that ground). `allocate()` outside a window still publishes immediately, so the
  one-block use cannot silently hand back non-executable code.
* On a cache miss the dispatcher opens one window, translates the entry block, translates its
  successors (fall-through, plus the branch destination for a `Jcc` — carried out of the translator
  as the new `BlockTranslation::alternate`), then publishes once. The prefetch is bounded: 16 blocks
  and 3584 generated bytes per window, so a wrong guess cannot translate an unbounded amount of dead
  code, and the byte budget is sized to fill the page the window touches (unused tail of a published
  page is unusable).
* `CachedBlock`/`BlockTranslation` gained `alternate`/`has_alternate`; `ExecArena::protection_changes()`
  counts the mprotect calls so this is reported, not asserted; `TranslationCache::prefetched_blocks()`
  counts speculative translations so the cost of a wrong guess is visible.

Measured on the AArch64 tree under qemu (deterministic counters, unlike the wall-clock figures):

| workload (x86-64 guest, translated by the AArch64 JIT) | blocks translated | mprotect calls | per block |
| --- | --- | --- | --- |
| 3-block micro workload (`hyperion_translate_bench`) | 3 | 2 | 0.67 |
| real guest, 551k retired instructions (`guest_probe`) | 1848 | 571 | **0.31** |
| same code before this change | any | 2 per allocation | 2.00 |

So the protection churn drops by roughly **6×** on a real guest. 1279 of the 1848 blocks were
translated ahead of execution, which is the cost side of the trade: about 1.4 blocks of speculative
work per miss on that guest, inside the byte budget.

The qemu wall-clock translation figures are too noisy to attribute a precise win (the same 3-block
workload measured 168 µs, 192 µs and 302 µs across runs, IQR fraction 0.15-0.38), so the honest
statement is: the number of protection changes — measured exactly — fell 6×, and on this x86-64 host
each `mprotect`-pair costs 2.787 µs per block, so the expected saving on hardware is ~1.9 µs per
translated block (~3.3 ms for a 1848-block guest). **No AArch64 hardware timing backs that estimate;
it is arithmetic on the host proxy plus the counter.**

Test consequences, as predicted, were smaller than the audit feared: the pinned block counts in
`test_translation_cache.cpp` and `test_translator.cpp` did **not** change (those guests already
translated every successor), so no assertion had to be updated. Two new tests pin the contract
instead: `exec_mem.arena_write_window_defers_one_mprotect_per_window` (portable: zero protection
changes inside a window, one per publish, none for an empty window) and
`exec_mem.arena_published_blocks_are_executable_and_reset_rearms_the_slab` (AArch64: published code
actually runs, and `reset()` makes the surviving slab writable again).

### 29.7 P7 done early — direct linking of block exits

§L item 7 (and §26 row 5) say every block exit goes back through the in-JIT hash
probe (hash, load pair, compare, branch: ~9 instructions) when it could be a single
`b`. That is now implemented, and it was done next because P5's write window makes it
nearly free: a block's page is writable until the window closes, so an exit can be
repointed at its successor without ever re-opening a published page.

How it works:

* The translator records an `ExitLink` for every exit whose successor is statically
  known — `jmp` with an immediate target and fall-through — as the byte offset of a
  branch word it emits as `br x24` (the shared dispatcher), plus the target guest PC.
* The cache, inside the same write window, translates the successors and then walks
  every block created in that window: each recorded site is overwritten with a direct
  `b <successor entry>`. `ExecArena::patch_instruction()` does the write and refuses
  any address that is published, misaligned or outside the arena, so W^X is never
  weakened; the page is simply flushed again by the `publish()` that follows.
* A word can only be repointed at one target, so a `Jcc` used to funnel both
  successors through one `CSEL`-selected exit — which can never be linked. It now
  emits two exit paths (`b.cond` to the other side), costing two extra words per
  conditional but making *both* sides linkable. Unlinked, the executed cost is
  unchanged: the dispatcher branch still runs exactly as before.

Measured on the AArch64 tree under qemu, one binary built with linking and one with
`resolve_links()` disabled, run alternately so host noise affects both:

| guest | linked | unlinked | effect |
| --- | --- | --- | --- |
| branch-heavy VM-style guest, 102M retired instructions | 6086 / 5891 / 6439 ms | 7567 / 7702 / 7470 ms | **-19 %** (medians 6086 vs 7567) |
| `hyperion_tier_bench` self-loop | 3.11 ns/ins | 3.09 ns/ins | no change (this workload is a self-loop superblock: it has almost no block transitions to link, so it cannot show the win) |

Link coverage on real guests: 1233 of 1605 exits linked on the VM-style guest, 1409 of
1835 blocks on the mixed guest. Generated code grows 2.5 % from the split-conditional
change. Both guests produce byte-identical output at both settings.

**Caveat, stated plainly:** these are qemu-user numbers for a translator that is itself
being emulated, on an x86-64 host. They show the change is a real and large win under
this measurement and that it does not regress the self-loop path; they are not an
AArch64 hardware measurement, which remains unavailable here.

### 29.8 P11 done — the remaining helper-call opcodes, chosen by measurement

The audit's item 11 was "inline the remaining integer helper opcodes, 15% of opcodes currently pay a
C++ call". That estimate was not used. Instead the question was answered by counting, with a
throw-away counter inside `jit_exec_complex` on a real guest (551k retired instructions, Tier 1 under
qemu): of 12 214 helper calls, **69 % were 8-bit `cmp` and 25 % 8-bit `test` plus 8-bit
`and`/`or`/`xor`** — together ~95 %. SSE2 was 3 %, `adc` 1 %, and `div`/`mul`/`cpuid`/`cmpxchg`
rounding errors. A second run printing operand shapes and flag masks showed why they were falling
back: all of them were **8- or 16-bit ALU with all six flags live** (mask `0x3f`), which the existing
inline path deliberately declined because AArch64 cannot set NZCV at 8/16 bits.

That is an inherent property, not a pessimistic liveness analysis: a block that ends without a
flag-consuming branch leaves every flag architecturally live, so byte compares cannot be treated as
"ZF only". So the fix inlines the full six-flag semantics instead of the opcode list:

* New `emit_flags_wide()` in `translator.cpp`. A 32-bit `SUBS` of two zero-extended bytes gets CF and
  ZF right — truncation cannot turn a non-zero byte into zero, and the borrow out of bit 7 is the
  borrow out of bit 31 — but it leaves SF on bit 31 (not bit 7) and OF at 32-bit width, where an
  8-bit overflow is invisible. SF, OF, AF and PF are therefore computed from the *truncated* result,
  with non-flag-setting instructions, so the NZCV-consuming stores can be emitted in any order.
* The 8/16-bit `cmp`/`test`/`and`/`or`/`xor` path in `emit_cached_body_insn` now emits the arithmetic
  and calls it, for every flag mask, with register, immediate, memory and high-byte operands. The
  result is only truncated when SF/OF/AF/PF is live or the result is stored.
* `TranslationCache::helper_calls()` and an `ExecContext::helper_calls` counter (slow path only, zero
  cost in generated code) so "this opcode is inlined now" is an assertion a test can make, printed by
  `hyperion run --tier=1`.

**The new test found a real bug in my first implementation**, which is worth recording: I used the
addition overflow identity `(a ^ r) & (b ^ r)` for subtraction as well. It agrees with the correct
`(a ^ r) & (a ^ b)` in most cases and disagrees exactly on the overflow cases — `0x00 - 0x80`
overflows a byte but not a word, so the 32-bit V flag is clear and nothing else catches it. The
table test below failed on the first sweep with `of` mismatched before the code was ever committed.

Measured effect, exact counters first (same guest, Tier 1 under qemu):

| guest | helper calls before | helper calls after | generated code before → after |
| --- | --- | --- | --- |
| branch-heavy VM-style, 102M retired insns | 9 012 090 | **257** | 642 140 → 612 244 B (−4.7 %) |
| mixed guest (fib, brk/mmap), 551k retired insns | 12 213 | **579** | 687 176 → 656 388 B (−4.5 %) |

Wall time, item 11 binary vs the P7 binary, run alternately, 3 rounds each, Tier 1 under
`qemu-aarch64-static` on this x86-64 host:

| guest | before (P7) | after | effect |
| --- | --- | --- | --- |
| branch-heavy VM-style | 6145 / 6006 / 6453 ms | **1770 / 1784 / 1812 ms** | **−71 %** (medians 6145 → 1784) |
| mixed guest | 336 / 330 / 338 ms | 380 / 330 / 329 ms | no change outside noise |
| `tier_bench` self-loop | 3.11 / 3.13 / 3.27 ns/ins | 3.08 / 3.13 / 3.14 ns/ins | no change |

The two null results are the honest part of the report. The mixed guest only ever made 12 213 helper
calls in a 551k-instruction run (2 %), and its 330 ms is dominated by process start-up, so nothing
should move — and nothing did. The self-loop benchmark contains no 8/16-bit ALU at all, so it cannot
show this win; that is the same reason it could not show P7's.

The 3.4× on the branch-heavy guest is large but explained by the counters: that guest is a bytecode
interpreter whose dispatch is a `switch` on `unsigned char`, so it was making one C++ helper call per
loop iteration (9.0 M of them), each one synchronising all of `GuestState` and doing a TLB-checked
byte load. It is a real workload shape, but it is the *best* case for this change: a guest that
compares bytes in its inner loop. Do not read 3.4× as a general figure. Correctness of that number
is separately established: both binaries retire exactly 102 139 313 guest instructions and print
`acc=9000001 counter=0`, which is what Tier 0 produces.

### 29.10 P9 (lazy flags) — measured, not started: it is an architectural change

P9 was investigated before any code was changed, and the measurement says this item
cannot be done the way the previous four were. Nothing was rewritten; this section is
the evidence and the proposed staged design.

#### How flags are represented today

`GuestState::flags` is six `bool`s (`guest_state.hpp:59-72`). They are stored to
individually as bytes (`strb` to `kCfOffset` … `kOfOffset`, `translator.cpp:77-82`),
and every consumer — `Jcc`, `Setcc`, `Cmovcc`, `Adc`/`Sbb`, and the C++ helper and
interpreter — reads them back from those bytes. There are three emitters:
`emit_flags_from_nzcv` (arithmetic), `emit_flags_logical`, and `emit_flags_wide` (the
8/16-bit one added in §29.8). Backward liveness (`flags_read_full`/`flags_written_full`)
already decides *whether* to materialise, and `live_out` at a block exit is `kFbAll`,
because the successor block's needs are not knowable at translation time.

#### What the profile says (tier 1 under `qemu-aarch64-static`, x86-64 host)

Counted with a throw-away profiler in the translator (translation-time events, so the
counts are exact and noise-free), then removed:

| | mixed guest (551k retired insns, 1721 blocks) | branch-heavy guest (102M retired insns) |
| --- | --- | --- |
| flag-materialisation events | **1483** | 1396 |
| … with all six flags (mask `0x3f`) | 1483 (**100 %**) | 1396 (100 %) |
| … at a block exit | 4 | 3 |
| producers with flags live | 1522 | 1433 |
| … whose materialisation is dead (consumed only by a fused `Jcc`) | **0** | **0** |
| words emitted for materialisation | **21 397** (14.4 per event) | 20 110 |
| … as a share of generated code | **11.7 %** | — |
| flag reads by consumers | cf 250, zf 1088, sf 102, of 70, **pf 20, af 20** | cf 241, zf 1032, sf 107, of 76, **pf 22, af 22** |

Three conclusions follow directly, and they are the reason this section exists:

1. **There is nothing to win inside a block.** Zero producers have a materialisation
   that a fused `Jcc` makes dead. The earlier hope — that `cmp; jcc` pairs materialise
   flags the fused branch then ignores — is false, because the flags are live *past*
   the branch: x86 keeps flags across `jcc`, so the successor block may read them. The
   cost is not in-block eagerness, it is the block boundary.
2. **PF and AF dominate the cost and are almost never read.** `emit_parity` is six
   instructions plus a store, and AF is four; together ~11 of the 14.4 words per event,
   i.e. roughly 76 % of flag code and ~8.9 % of all generated code — for flags that
   consumers read 20 times in a 551k-instruction run. The only in-program readers
   found are variable shifts and string ops, which read `kFbAll` only because
   `flags_read_full` is conservative for them (`cl == 0` leaves flags untouched).
3. **PF and AF are observable outside generated code.** A throw-away build that simply
   stopped computing them did not just change flags: the branch-heavy guest's retired
   instruction count moved from 102 139 313 to 102 145 026. The helper path
   (`execute_cached_helper` → `execute_decoded`) reads `GuestState.flags` as
   materialised bools, so any deferred representation leaks into C++ the moment a
   helper runs. That is the correctness surface, and it is real rather than
   theoretical.

#### Why that makes P9 architectural

A lazy scheme has to carry a descriptor (operation kind + operands) instead of six
materialised bytes, which changes what `GuestState::flags` *means* between the moment a
producer runs and the moment something reads them. Concretely it touches:

| surface | why | count |
| --- | --- | --- |
| `GuestState` + its offset asserts | new pending state; `flags` no longer always authoritative | `guest_state.hpp`, `translator.hpp:136-146` |
| flag emitters | store the descriptor instead of PF/AF (and for a full scheme, all six) | 3 functions, 7 call sites |
| forced materialisation | must run before anything that reads the bytes | shared exit epilogue (1), every helper-call site, every non-fused flag consumer |
| invalidation | helpers rewrite `flags` and know nothing about the descriptor | helper call return path |
| interpreter / tier 0 | must never observe a pending descriptor | unchanged only if every JIT→C++ exit forces |

The good news for staging: every JIT→C++ transition already funnels through a single
shared epilogue (`translator.cpp:2166`, reached from the syscall, halt, fault, budget
and dispatcher-miss paths at `:3358`, `:3367`, `:3451`, `:3570`, `:2152`), so
"force on exit to C++" is one site, not five. Helper calls are the other site, and P11
has made those rare (257–579 per run on these guests).

Per the instruction to stop rather than half-implement an architectural change, no P9
code was written. The staged design proposed:

**Stage 0 — cheaper materialisation (no semantics change, ~1 day).** Replace
`emit_parity`'s six-instruction fold with a 256-byte parity table (`adrp`/`ldrb`, or a
table pointer hoisted into `ExecContext`), and skip the `eor`/`eor` prefix of AF when
`lflags & kFbAf` is clear. Strictly local, no new state, verifiable with the existing
differential tests. Expected: ~4-5 words off each of ~1483 materialisations
(~3 % of generated code), **no claim about runtime** until measured.

**Stage 1 — deferral of PF and AF only (~2-3 days).** These are the expensive flags and
the rarely-read ones. Producer emits a 3-byte descriptor (`result_low8`, `xor_low8`,
`pending`) instead of ~11 words of parity/AF code; a `force_pf_af` sequence (~11 words)
materialises on demand and clears `pending`. Forced at: the shared exit epilogue, before
every helper call, and at any consumer whose condition needs PF (`jp`/`jnp`) or that
reads AF. Soundness argument: PF = parity(result low byte) and AF = bit 4 of
`a ^ b ^ r`, both recoverable from the two bytes; every other flag is untouched, so
`Jcc`/`Adc`/`Sbb` and tier 0 are unaffected. Test plan: extend the §29.8 differential
table with `jp`/`jnp`/`setp`/`lahf`, sequences that put a helper call between a producer
and a PF consumer, and a producer immediately followed by a block exit.

**Stage 2 — full six-flag descriptor (QEMU-style `cc_op`/`cc_src`/`cc_dst`, ~1 week).**
Only worth it if Stage 1's measurements show the remaining four flags matter; the
profile above says they are read on 1-2 % of producers, so the ceiling is the ~3 words
per event that CF/ZF/SF/OF cost. This is the version that would need the invalidation
protocol above in full.

### 29.11 P9 stage 1 done — PF/AF deferral behind a 3-byte descriptor

§29.10 measured P9 and concluded that a full six-flag descriptor is architectural. The
following is the cheapest version that is *not*: defer exactly the two flags that are
expensive to produce and almost never read, and keep the other four eager. Per the
stage plan in §29.10 this is stage 1 only — no `cc_op`/`cc_src`/`cc_dst`, no P8, no P14.

#### The descriptor

`GuestState` gains three bytes in what used to be alignment padding after `df`, so no
existing layout offset moves:

```c++
std::uint8_t pf_af_pending = 0;  // bit 1 = PF stale, bit 2 = AF stale
std::uint8_t pf_af_res_low = 0;  // result & 0xFF
std::uint8_t pf_af_xor_low = 0;  // (a ^ b ^ result) & 0xFF; bit 4 is AF
```

Both flags come out of those two bytes for addition *and* subtraction, because bit 4 of
`a ^ b ^ r` is exactly the carry/borrow into bit 4 — the same identity the eager code
already used for AF. Logical ops store a zero xor byte (x86 clears AF there). The
pending bits deliberately use the same numbering as the translator's flag mask
(`kFbPf` = bit 1, `kFbAf` = bit 2), so one value both selects what to write and records
what is stale. A producer emits **4 instructions** (two `strb`, `movz`, `strb`) instead
of the ~9 that PF+AF cost (5-instruction parity fold + 3 for AF + stores).

#### Forced materialisation — audited, not assumed

Every site that can read or write `GuestState::flags` was enumerated before writing code:

| writer | where | treatment |
| --- | --- | --- |
| inline ALU (`emit_one` 32/64-bit, imm-folded, INC/DEC) | `translator.cpp` fast path | descriptor (`emit_pf_af_store`) |
| inline ALU (block-level 32/64-bit, NEG/INC/DEC) | `kAdd..kTest`, `kNeg..kDec` | descriptor |
| inline 8/16-bit ALU (P11) | `emit_flags_wide` caller | descriptor; bit 4 and the result low byte are width-independent |
| self-loop / fast-loop exit flags | 2 specialisation paths | descriptor |
| helper (`execute_cached_helper` → interpreter) | C++ | writes `flags` directly; pending is flushed on entry |
| `emit_flags_from_nzcv` / `emit_flags_logical` / `emit_flags_wide` | shared | PF/AF removed from all three; only CF/ZF/SF/OF are emitted eagerly |

| reader | where | how it is forced |
| --- | --- | --- |
| `JP`/`JNP` branch | `emit_cond_branch` | `emit_pf_af_guard` before the `ldrb` of PF |
| `SETP`/`SETNP` | `emit_eval_cond_to_reg` | `emit_pf_af_guard` |
| interpreter / syscalls / tests / fork / trace | C++ after `cache.run()` | shared exit epilogue flush (`build_jit_trampolines`) and `emit_return` for the direct-`ret` path |
| any helper re-entering the interpreter | `jit_exec_complex` | `flush_pending_flags()` before the call |
| `LAHF`/`SAHF`/`PUSHF`/`POPF` | — | not implemented by this front end, so there is no such consumer (`grep` for `kLahf`/`kPushf`/`kSahf` is empty) |

The forced points are therefore three, not five: **exit epilogue**, **helper boundary**,
**PF/AF consumer**. Two properties make them sufficient:

* The dispatcher's jump-table hit path (`br xT3`) stays inside generated code and does
  *not* flush, so a linked transition carries the descriptor forward — that is where the
  saving comes from.
* Every path that leaves generated code for C++ (block return, halt, syscall, fault,
  budget exhaustion, dispatcher miss) reaches the same exit-epilogue trampoline, so the
  "never let C++ see a stale flag" rule is one site. A test asserts
  `pf_af_pending == 0` after every run, so a future path that bypasses it fails loudly.

AF is *not* representable on its own: PF and AF are always written as a pair by every
instruction this front end models, so `live_flags & (kFbPf|kFbAf)` is all-or-nothing.
`emit_pf_af_store` handles the impossible partial case by materialising immediately
rather than leaving half a descriptor live, and `flush_pending_flags` (the C++ mirror)
clears only the bits it materialised. Nothing here weakens a test: the descriptor is
never allowed to be the answer, it is only allowed to be *late*.

#### Measurements

Both guests are the same unmodified binaries used in §29.7/§29.8, run under
`qemu-aarch64-static` on the x86-64 host with `env -i` (`env` matters: a different
environment changes the guest's own libc start-up, which shifts every count — that is
why an earlier A/B of this change looked like a 100-block difference). Baseline is
`0d2a00d` built with the same compiler, flags and CMake options. Five interleaved runs
each.

| workload | metric | before | after |
| --- | --- | --- | --- |
| `/tmp/vm_guest_x64` (branch-heavy bytecode interpreter, 102 134 745 retired) | generated code | 604 696 B | **589 196 B** (−2.6 %) |
| | PF/AF descriptor stores executed | — | 30 025 889 |
| | PF/AF **materialisations** executed (exit epilogue 465 + helper boundary 110) | ≳ 30.0 M parity folds | **575** |
| | helper calls | 214 | 214 |
| | wall clock, 2 rounds of 5 (median) | 1 026 / 1 096 ms | 1 060 / 1 054 ms |
| | guest output / retired instructions | `acc=9000001 counter=0` / 102 134 745 | identical |
| `/tmp/guest_probe_x64` (mixed C workload, 546 821 retired) | generated code | 649 428 B | **632 452 B** (−2.6 %) |
| | descriptor stores executed | — | 75 407 |
| | PF/AF materialisations executed | ≳ 75 k | **652** (526 + 126) |
| | helper calls | 252 | 252 |
| | wall clock, 2 rounds of 5 (median) | 226 / 236 ms | 249 / 269 ms |
| `hyperion_tier_bench` (self-loop, tier 1) | ns per guest instruction (3 runs) | 1.881 / 1.789 / 1.714 | 1.965 / 1.643 / 1.730 |

Reading these honestly:

* **The win is code size and work removed, not wall clock.** −2.6 % of generated code,
  and the number of executed PF/AF materialisations collapses by ~4-5 orders of
  magnitude (30.0 M → 575), because the parity fold now runs only when something can
  observe PF/AF instead of at every producer execution.
* **No throughput claim.** Wall clock moved in *both* directions across two rounds of
  five interleaved runs (+3 % and −4 % on `vm_guest`, +10 % and +14 % on
  `guest_probe`), while single runs of one unchanged binary span 208-370 ms. The
  difference is inside the noise, and `hyperion_tier_bench` (1.789 → 1.730 ns per guest
  instruction, medians of three) did not regress. Under qemu-user the host's own
  translation dominates, so instructions removed from generated code are close to
  invisible; this is a size-and-work result until it is measured on real AArch64
  hardware.
* The 4-instruction descriptor store is *cheaper* than the 9 it replaces for a producer
  whose PF/AF is never read, and the extra 3 instructions in the exit epilogue only run
  when a descriptor is actually pending.

#### Tests

Ten differential tests (`tests/unit/test_translation_cache.cpp`, `pending_pf_af_*`,
`pf_consumer_*`, `setp_and_setnp_*`, `helper_boundary_*`, `partial_flag_update_*`) run
each program through tier 0 and tier 1 and require the *entire* architectural result —
all 16 GPRs, `rip`, step count and all six flags — to agree, then assert
`pf_af_pending == 0` and that the case really did defer something. Coverage per forcing
path:

| forcing path | test | what fails without the forcing |
| --- | --- | --- |
| exit epilogue, AF=1 | `pending_pf_af_is_materialised_at_the_exit_epilogue` | incoming PF/AF are the opposite of the result, so a skipped flush gives the wrong bits |
| exit epilogue, AF=0 | `pending_pf_af_af_zero_case_reaches_the_exit_epilogue` | AF must be *cleared*, not left set |
| consumer in the same block | `pf_consumer_forces_materialisation_inside_a_block` (taken and not-taken) | JP takes the wrong side, `rcx` differs |
| `SETP`/`SETNP` | `setp_and_setnp_force_materialisation` | `al`/`bl` hold the stale PF |
| helper between producer and consumer | `helper_boundary_forces_materialisation_of_pf_and_af` | PF/AF after `div` come out stale |
| helper *writes* PF | `helper_writing_pf_cannot_resurrect_a_stale_descriptor` | the exit flush overwrites the helper's PF with the producer's |
| partial update (INC keeps CF) | `partial_flag_update_keeps_cf_and_defers_pf_af` | CF corrupted, or the descriptor clobbers a flag INC does not write |
| self-loop + link | `pending_pf_af_survives_a_self_loop_and_a_link` | descriptor lost across the fast-loop back edge |
| direct-linked exit | `pending_pf_af_crosses_a_directly_linked_exit` | descriptor lost across a linked transition |
| consumer in the successor block | `pf_consumer_in_the_successor_block_forces_materialisation` | PF read before it is materialised after a block transition |

Each test was verified to be able to fail, by mutating the implementation and rebuilding:

| mutation | outcome |
| --- | --- |
| remove the `jit_exec_complex` flush | `helper_writing_pf_*` fails on PF (1 vs 0), `helper_boundary_*` fails on the flush counter |
| remove the exit-epilogue and `emit_return` flush | 3 tests fail, incl. `pf_af_pending == 6` observed by C++ |
| remove both consumer guards | 4 tests fail (JP direction, `setp`, self-loop, successor block) |
| (accidentally, during development) `tbz` on the wrong pending bit | 5 tests fail — the guards became inert |

#### Verification

* native (x86-64): 341 passed / 0 failed / 38 skipped of 379; `ctest` 19/19
* AArch64 under `qemu-aarch64-static`: 379 passed / 0 failed / 5 skipped of 384
* `clang-format-19` clean on every changed file
* strict W^X, P5 successor prefetch and P7 direct linking untouched; the descriptor lives
  in plain `GuestState` memory, so no new executable mapping, and no new helper call.

Platform limit, stated rather than implied: the JIT is AArch64 and was executed under
`qemu-user` on an x86-64 host. That validates semantics, counts and code size; it does
not measure AArch64 pipeline behaviour, where removing 5 instructions per producer is
expected to matter more than it does here.

Not done, deliberately: CF/ZF/SF/OF stay eager (§29.10 stage 2), no IR, no P8, no P14.

### 29.12 P6 done — hot-block gating, and the crossover that justifies it

Item 6 of §L ("is there hot-block detection / tiering?") has two halves. The second half
— an execution profile that feeds decisions — did not exist: every basic block was
translated on its **first** reach, however cold. The measurement below says when that is
the wrong call, and the change is the smallest one that acts on it.

#### What was measured first

The translator was temporarily instrumented (never committed) to print, per block, the
number of guest instructions reached before the next miss, i.e. the number of times each
compiled block actually executes. Then the two tiers were compared on the same guests at
fixed step budgets (`/tmp/vm_guest_x64`, an add/`jmp`-dominated loop; `qemu-aarch64`,
min of 3, whole process):

| guest instructions | tier 0 | tier 1 (compile on first reach) | ratio |
| --- | --- | --- | --- |
| 1 000 | 48.3 ms | 73.0 ms | 0.66× (tier 1 **slower**) |
| 10 000 | 55.0 ms | 173.6 ms | 0.32× (tier 1 **slower**) |
| 100 000 | 80.0 ms | 153.7 ms | 0.52× |
| 1 000 000 | 296.8 ms | 167.3 ms | 1.77× |
| 10 000 000 | 2445.7 ms | 239.3 ms | 10.2× |

The crossover is tens of thousands of guest instructions. Below it, translation plus the
W^X publish (§29.6 — two `mprotect` calls per write window) costs more than the work it
saves, and the guest pays that cost even when it is a `--help`-style run that halts after a
few thousand instructions. A block reached once is pure loss; nothing tracked how often a
block was reached, so the cache could not tell the two apart.

#### What was implemented

`TranslationCache` keeps a per-PC saturating counter and answers the first
`hot_threshold` reaches of a block start with **one step of the reference interpreter**:

* `hot_` is a direct-mapped table of 1024 `std::uint16_t` counts, indexed by
  `(pc ^ (pc >> 12)) & 1023`. A collision only makes a count *lower* than the truth, which
  means "translate a little later" — never a missed hot block and never a wrong answer, so
  the table needs no tag, no eviction and no synchronisation.
* `should_gate(pc)` is checked in `translate_at()` **after** the cache-miss lookup, so a
  compiled block is never re-checked and dispatch hits are untouched.
* A gated step runs `hyperion::step()`, the same code tier 0 runs, so tier-1 cold semantics
  are tier-0 semantics by construction. It accounts for the step budget exactly like a
  compiled block (`RunStatus::kStepLimit`), counts the guest instruction, bumps
  `unimplemented_syscalls` the same way, and repeats the mapping-epoch TLB flush and
  code-epoch `invalidate_all()` bookkeeping a compiled block would have done for a
  `syscall` — a gate must not become a way to skip a invalidation.
* `kDefaultHotThreshold = 2` (translate on the third reach); `0` restores the old
  behaviour exactly, and `--hot-threshold=N` exposes both. Counters
  `gated_steps()` / `promoted_blocks()` are printed by `hyperion run`.
* P5 successor prefetch is deliberately **not** gated: a prefetched block exists because
  its predecessor is already hot, and gating it would defeat the prefetch it was added for.
  P7 direct linking is unaffected (links are patched between blocks that both exist).

#### Effect — same guests, 5 interleaved repetitions, median/min of whole-process wall clock

`t1-thr0` is the previous behaviour (compile on first reach); `t1-thr2` is gating.
Every run includes the ~45 ms `qemu-aarch64` process startup, which is why the small
budgets move so little.

| guest | steps | tier 0 | t1-thr0 | t1-thr2 | gated/old |
| --- | --- | --- | --- | --- | --- |
| vm_guest | 1 000 | 54.4/51.4 | 69.1/68.2 | 66.5/59.7 | 0.96× |
| vm_guest | 10 000 | 58.4/57.0 | 149.9/134.5 | 104.2/99.5 | 0.70× |
| vm_guest | 100 000 | 86.0/81.5 | 158.4/145.7 | 109.9/103.2 | 0.69× |
| vm_guest | 1 000 000 | 276.3/272.0 | 157.2/149.7 | 102.2/97.3 | 0.65× |
| vm_guest | 10 000 000 | 2231.4/2065.5 | 224.0/215.6 | 177.2/174.1 | 0.79× |
| call_guest | 1 000 | 55.1/52.7 | 64.4/60.1 | 65.6/62.8 | 1.02× |
| call_guest | 100 000 | 74.8/73.6 | 151.0/149.9 | 116.7/114.3 | 0.77× |
| call_guest | 1 000 000 | 280.5/239.6 | 190.4/176.9 | 137.9/133.8 | 0.72× |
| call_guest | 31 300 000 (full run) | 6709.6/6267.2 | 1088.7/1022.0 | 938.2/858.0 | 0.86× |

Gating is never slower than compiling eagerly (the two budget-1k rows are within
run-to-run noise: the *same* guest, measured 1.02× and 0.96×), and it is 14–35 % faster
from 10 000 instructions upwards, which is where a guest spends its time. Subtracting the
~45 ms startup on the 1 M-step vm_guest run: 112 ms → 57 ms of actual work.

Static cost, same run (1 M steps of vm_guest):

| | t1-thr0 | t1-thr2 |
| --- | --- | --- |
| blocks translated | 1237 | **423** |
| generated code | 480 828 B | **150 288 B** |
| block starts translated (guest instructions compiled) | 6051 | **1951** |
| `mprotect` calls | 394 | **130** |
| translation wall time | 82.0 ms | **34.1 ms** |
| guest instructions interpreted while cold | 0 | 5187 (0.52 %) |
| blocks promoted | 0 | 130 |

So the change buys a 69 % smaller code footprint and a 2.4× cheaper translation phase,
and pays 0.5 % of the run in interpreted instructions. Full-run guest output is unchanged
(`acc=9000001 counter=0` and `acc=20475694797959 g=50991808` on both tiers).

#### Tests

`tests/unit/test_translation_cache.cpp`, five new differential/gating tests (each with the
usual AArch64-host `HYPERION_SKIP` stub):

| test | pins |
| --- | --- |
| `gating_does_not_translate_a_cold_block` | 3-instruction program: `block_count() == 0`, `promoted_blocks() == 0`, `gated_steps() == 3`, state identical to tier 0 |
| `gating_promotes_a_hot_loop` | 9-iteration, 18-instruction branch-heavy loop: ≥ 2 promotions, ≥ 2 blocks, `0 < gated_steps() < steps`, state identical to tier 0 |
| `gating_zero_threshold_translates_immediately` | `--hot-threshold=0` reproduces the old behaviour exactly (`gated_steps() == 0`) |
| `gating_results_match_across_tiering_transitions` | thresholds 0, 1, 2, 3, 1000 over the same program all produce the identical architectural state |
| `gating_accounts_cold_steps_against_the_step_limit` | `max_steps = 4` stops mid-prefix: `kStepLimit`, `steps == 4`, `gated_steps() == 4`, nothing translated |

The 14 pre-existing failures the change caused were all tests that assert *generated-code*
properties (exact word counts, helper counters, PF/AF descriptor bookkeeping) from a single
pass over a 2–15-instruction program. They are correct to fail — with gating there is no
generated code to measure — so each now calls `set_hot_threshold(0)` with a comment saying
why: the default is not weakened to make tests pass, and the counters they assert on can
still observe real behaviour.

#### Verification

* native (x86-64), clang-19 RelWithDebInfo: 341 passed / 0 failed / 43 skipped of 384
  (5 new skips are the new AArch64-only tests); `ctest` 19/19
* `HYPERION_WARNINGS_AS_ERRORS=ON` with GCC 13 and clang-19: clean, 341/0/43 each
* AArch64 (cross, `qemu-aarch64-static`): **384 passed / 0 failed / 5 skipped of 389**
* full guests, both tiers, end to end: identical output (§ above)

Platform limit: as in §29.11, this is `qemu-user` on an x86-64 host. Absolute times are
emulated and startup-dominated; the ratios are the result. On real AArch64 hardware the
translation-side win (fewer `mprotect` calls, less code) should be larger, not smaller.

Rejected while investigating: gating *prefetched* successors; evicting cold blocks from a
bounded cache (the cache is unbounded and code size is already down 69 %); an adaptive
threshold based on translation-time-per-instruction (needs the profile P14 would build).

### 29.13 P10 — measured: link the `call`, and why there is no shadow stack

Item 10 of §L proposed a shadow stack for `ret`. The measurement below says the money in
the call/return path is on the **call** side, not in re-reading the return address, and
that a shadow stack is the wrong mechanism for the remaining part.

#### How often, and what it costs today

Opcode histograms of the benchmark guests (tier 0, counted, not estimated):

| guest | guest instructions | `call` | `ret` | share of all instructions |
| --- | --- | --- | --- | --- |
| `call_guest` (small non-inlined functions, recursion) | 31 345 692 | 3 200 587 | 3 200 587 | **20.4 %** |
| `guest_probe` | 555 896 | 3 256 | 3 256 | 1.2 % |
| `vm_guest` (loop-heavy) | 102 143 873 | ~2 000 | ~2 000 | 0.002 % |

Before this change every one of those exited through the shared in-JIT dispatcher
(§9), which is 11 instructions plus two jump-table loads:

```
str x9,[state,#rip]      ; publish the target pc
lsr x10,x9,#12  / eor / ubfx        ; 1024-entry direct-mapped index
add/ldp x11,x12,[ctx+jump_table]    ; tag + target block
cmp x11,x9 / b.ne miss / br x12
```

For a `call`, the destination is known at translate time whenever it is an immediate —
it is a block start, exactly like a `jmp` destination. Nothing about the shadow stack was
needed to remove that dispatch, and removing it is a 3-word patch to the emitter because
§29.7's machinery already does the hard parts (patch the branch word when the successor
appears, stay inside a 26-bit displacement, forgets everything on invalidation).

#### What was implemented

`call imm32` exits are registered as `ExitLink`s exactly like `jmp` exits, with the `rip`
store emitted locally instead of relying on the dispatcher. That is +1 word per call site
and −11 words per executed call. Indirect calls (`call r/m`) and `ret` keep the dispatcher,
because their destination only exists at run time.

#### Effect — same guest, interleaved, 15 repetitions, whole-process wall clock

| guest | before | after | ratio |
| --- | --- | --- | --- |
| `call_guest` full run (31.3 M guest insns) | 967.0 ms median, 909.1 min, 1034.4 max | **878.3 ms median, 733.7 min, 917.5 max** | **0.908× median / 0.807× min** |
| `guest_probe` full run (556 k) | 139.9 ms median | 142.6 ms median | 1.02× (within noise; only 3256 calls) |
| `vm_guest` 10 M steps | 188.7 ms median | 187.4 ms median | 0.993× (no calls, as expected) |
| generated code, `call_guest` | 183 820 B | 184 040 B | +0.12 % |
| linked exits, `call_guest` | 379 | **525** | +146 call sites |

`qemu-user` caveat as before: absolute numbers are emulated and startup-heavy; the ratios
and the counters are the result.

#### The shadow stack: rejected, with the reasoning and the size of what is left

A shadow stack keeps a host-side copy of each pushed return address so `ret` need not read
guest memory. Two ways to use it, and neither survives contact with the requirements:

1. **Trust it.** Unsound, and not marginally so. A guest may rewrite its own return
   address (`push <pc>` + `ret`, a classic obfuscation and a real calling convention in
   some hand-written asm), `ret imm16` changes the stack by more than one slot, and `rsp`
   can be changed between the call and the return by anything from `leave` to a signal
   frame to the reference interpreter taking over for one instruction. The pending
   descriptor in §29.11 is safe *because* it is a pure function of architectural register
   values that generated code just produced; a stack cache is not.
2. **Verify it against the architectural stack.** Then the load stays, `rsp` bookkeeping
   stays, and the only thing saved is the pc-to-block lookup — a *return-target cache*, not
   a shadow stack. Its ceiling is measurable from the numbers above: linking removed the
   11-word lookup from 3.2 M calls and bought 9 % of the run (median), so removing ~6 of
   those 11 words from 3.2 M rets is worth roughly **4–5 % of a call/return-heavy guest,
   and nothing at all on a loop-heavy one**. Against that: per-site writable state (strict
   W^X forbids patching a tag into the code page, §29.6), a slot-index allocator, two
   stores on the shared dispatcher's hit path for *every* dynamic exit, and a full flush of
   those slots on every code-epoch invalidation — because a stale slot is not a slow
   answer, it is a jump to the wrong block. The same cache would also serve `jmp r/m` and
   `call r/m`, which is a property of the branch architecture rather than of `ret`, so it
   belongs with the P14 work (an IR with a real indirect-branch representation) where it
   can be built and tested once for all three.

Decision: implement the call side (measured, low risk, no new state), defer the verified
return-target cache to P14, and record the rejection. **That deferral has since been resolved in
§29.21, which measures `ret` at ≤3.9 % of retired instructions on real workloads and therefore
rejects the cache outright** — the 4–5 % figure above came from a synthetic guest where CALL/RET
were 20.4 % of instructions, and is corrected there. Reopening it needs a measurement
that shows the ret side is worth more than the ~4–5 % ceiling above.

#### Tests

| test | what it pins |
| --- | --- |
| `direct_call_exits_are_linked_and_return_correctly` | two `call imm` sites: result matches tier 0 **and** `linked_exits() >= 1`; verified to fail (`CHECK failed: got.linked >= 1`) with the emitter change reverted |
| `a_rewritten_return_address_is_honoured` | the guest pushes its own return addresses and `ret`s to them; the skipped `mov eax, 99` after each `ret` must never execute. This is the guard that fails if anyone ever adds a trusted shadow stack |
| `recursive_calls_match_the_interpreter` | four nested calls with a base case: linked *and* unlinked call exits, and `rsp` back at its start |
| `ret_imm_discards_bytes_from_the_guest_stack` | `ret 8` pops the return address and discards a caller slot: `rsp` and every register match tier 0 |

#### Verification

* native (x86-64) clang-19: 341 passed / 0 failed / 47 skipped of 388; `ctest` 19/19
* AArch64 (`qemu-aarch64-static`): **388 passed / 0 failed / 5 skipped of 393**
* full runs on both tiers still print `acc=20475694797959 g=50991808` and
  `acc=9000001 counter=0`

### 29.14 P9 stage 2 (CF/ZF/SF/OF) — measured, and not started: the ceiling is inside the noise

§29.10 profiled lazy flags and concluded that a full six-flag descriptor is an architectural
change; §29.11 implemented the cheap half (PF/AF). This section answers the stage-2 question
with numbers: *how much of the remaining flag cost could a `cc_op`/`cc_src`/`cc_dst`
descriptor actually remove, and can this environment measure it?*

#### What is left after stage 1

With PF/AF deferred, a flag producer still materialises each remaining live flag with two
instructions (`cset` from NZCV, `strb` into `GuestState::flags`), so CF+ZF+SF+OF is up to
8 instructions. A temporary probe (never committed) walked the per-step flag liveness of
every translated block of each benchmark guest and counted the generated words each step's
`live_flags` implies — 2 per live CF/ZF/SF/OF, 4 for the PF/AF descriptor — against the
block's total size:

| guest | blocks | generated words | flag words | share | of which at the block's last step | flag-writing steps |
| --- | --- | --- | --- | --- | --- | --- |
| `call_guest` | 542 | 51 336 | 5 360 | **10.44 %** | 0 | 469 |
| `vm_guest` | 526 | 48 759 | 5 256 | **10.78 %** | 0 | 459 |
| `guest_probe` | 663 | 64 220 | 6 554 | **10.21 %** | 0 | 569 |

Two facts matter. Flag materialisation is ~10.4 % of generated code on all three guests,
i.e. roughly **11.4 words per flag-writing step** — and *none* of it happens at a block's
last step: a block ends at a terminator (`jmp`/`jcc`/`call`/`ret`/`hlt`), which writes no
flags, so every producer is mid-block and its flags are live to the block boundary (the
liveness pass starts from `live_out = kFbAll`, §29.10). Nothing is being materialised
"early" in any sense that a smarter liveness pass could remove; it is materialised because
the block boundary demands it.

#### The ceiling, and why it is not enough

A `cc_op`/`cc_src`/`cc_dst` descriptor would replace the 8 CF/ZF/SF/OF words with ~3-4
stores (op+width packed into one byte, two 64-bit operands), so the deferrable part is
about **4.5 of the 11.4 words per producer, ≈ 4 % of executed generated code** — and that is
an upper bound: it assumes every reader is either fused (the `cmp`+`Jcc` case, already fused)
or paid for elsewhere. Then the calibration problem starts:

* the same environment was used to A/B the *existing* PF/AF deferral, by rebuilding with
  `emit_pf_af_store` materialising immediately (a 3-line temporary change that keeps
  everything else identical). Two binaries, `call_guest` full run, 15 interleaved pairs:
  median 777.8 ms (eager) vs 807.6 ms (deferred), **paired-ratio median 1.021**, per-pair
  ratios 0.89 – 1.21. The effect of removing ~5 words per producer is *inside* this
  environment's noise;
* therefore a change whose ceiling is ~4 % cannot be validated here at all. Every
  correctness risk would be taken on faith, which is the one thing this project does not do.

So: **not implemented, and the reason is measurement, not effort.** A `qemu-user` build on
an x86-64 host, with `mprotect`-dominated translation and no AArch64 pipeline model, cannot
resolve a 4 % change; the earlier claim in §29.10 that lazy flags are "architectural" stands,
and stage 1 was the only part with a cost worth removing.

#### What implementing it would actually require

Recorded so the decision can be revisited rather than re-derived. A descriptor is only the
easy half; the surface is every place a flag can be produced or observed:

* producers with non-obvious carry: `ADC`/`SBB` (CF is an *input*), shifts and rotates
  (CF = last bit shifted out, OF defined only for shift-by-1), `MUL`/`IMUL` (CF/OF defined,
  SF/ZF/AF undefined), `DIV`/`IDIV` (all undefined), `BSF`/`BSR` (ZF), `BT`/`BTS`
  (CF), the decimal adjust group, `CLC`/`STC`/`CMC`, `LAHF`/`SAHF`, `PUSHF`/`POPF`;
* consumers: all 16 conditions through `Jcc`, `CMOVcc`, `SETcc`; `ADC`/`SBB` carry-in;
  `RCL`/`RCR`; the helper boundary (helpers read and write `GuestState::flags` directly);
  the interpreter cold path and every unsupported opcode; `sigreturn`/signal delivery
  (RFLAGS is pushed); fault reporting;
* and it must be *exactly* the reference interpreter's answer, including the flags x86
  calls undefined, because the whole test strategy is differential against tier 0.

The defensible way in is to make flag effects explicit in an IR (§29.18, P14) and generate
the descriptor from one table instead of from ~30 emitter sites. Revisit trigger: either a
native-AArch64 measurement setup with ≤1 % noise, or P14 landing with a flag-effect table.

### 29.15 P8 — measured: there is almost nothing to keep across a block boundary

Item 8 of §L ("cross-block register allocation") assumed the block boundary is where the
register traffic is. A temporary probe (never committed) counted, over every translated
block of each benchmark guest, the generated words attributable to *crossing a boundary*:
the prologue's guest-register loads, every `spill_ra()`/`reload_ra()` the block emits (exit
spills, helper boundaries, fault exits), against the block's total size. It also counted
how many guest registers the allocator actually pins per block.

| guest | blocks | generated words | prologue | exit/helper spills | reloads | boundary share | allocated regs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `call_guest` | 542 | 51 336 | 625 | 2 002 | 498 | **6.09 %** | 360 (0.66/block) |
| `vm_guest` | 526 | 48 759 | 594 | 1 900 | 482 | **6.10 %** | 343 (0.65/block) |
| `guest_probe` | 663 | 64 237 | 927 | 2 675 | 626 | **6.58 %** | 494 (0.75/block) |

The reason the number is small is the allocator itself: it pins a guest register only when
the block references it more than twice (`refs[i] > 2`, "strictly saves ≥ 1 word over 1 load
+ 1 store"), and at ~4.7 guest instructions per block that condition is rarely met — **0.65
to 0.75 registers per block**. A block therefore pays about one `ldr` on entry and one `str`
per exit for guest state, and the rest of its operands already go straight to and from
`GuestState`.

#### The smallest safe design, and why it still is not worth it

The design that would remove that traffic is a *canonical* assignment: a fixed guest→host
map used by every block (so a linked successor needs no reload), guest values written back
only at the C++ funnel (the exit epilogue stores the whole canonical set — cheap, because
C++ exits are rare), and link patching refusing to link blocks whose assumptions disagree.
It is incremental in the sense that it does not touch the emitter, only the prologue,
the exit epilogue and the allocator.

But the prize is the 6 % above, and only part of it is removable: a linked successor still
needs values its predecessor did not keep, the exit epilogue has to store the canonical set
anyway, and helper boundaries must keep the write-back discipline that §29.3 established
(that is the invariant P3 retracted, and it is not negotiable). Realistically 3–4 % of
generated code — below the measurement resolution established in §29.14 (paired-ratio spread
0.89–1.21 for a 5-word-per-producer change), for a redesign of the one invariant that makes
guest state visible at every exit, helper call, fault and signal.

Deferred, with a trigger that is honest about *why* the number is small: if the allocator is
ever made more aggressive (always pinning `rsp`, `rbp`, `rax`, say, which is a separate
decision with its own cost), the boundary cost rises and this becomes worth measuring again.
As it stands, P11 (inlining the remaining helper opcodes) and P6 (not translating cold
blocks) attacked the same overhead from the side that actually pays.

### 29.16 P12 — signals: what exists, what was fixed, and the gap that stays a gap

Item 12 of §L asks for an audit before anything else, and the audit is the reason this item
is smaller than it looks: the missing part is not a policy, it is an x86-64 signal frame.

#### Audit — what a guest can do today

| mechanism | before this change | after |
| --- | --- | --- |
| `rt_sigaction`(13) | records and reports dispositions; rejects `SIGKILL`/`SIGSTOP` and a wrong `sigsetsize` | unchanged |
| `rt_sigprocmask`(14) | `SIG_BLOCK`/`SIG_UNBLOCK`/`SIG_SETMASK` stored in a guest mask; an invalid `how` was silently treated as "set" | invalid `how` now `-EINVAL`; the mask change re-evaluates pending signals |
| `sigaltstack`(131) | records and reports the alternate stack | unchanged (still never *used*: nothing is delivered) |
| `kill`(62)/`tgkill`(234) | **any non-zero signal ended the run** with `128+signo`, whatever the guest had installed | decided by the guest's disposition and mask — see below |
| handler invocation | never | never (documented, with the reason) |
| `rt_sigreturn`(15) | `-ENOSYS` | unchanged; unreachable while nothing is delivered |
| asynchronous delivery | none: no host signal is forwarded, no signal can arrive mid-block | unchanged, and stated as an invariant (see below) |

#### What was implemented

`kill`/`tgkill` aimed at this guest (the only target the emulator has) is now a *synchronous
self-raise* evaluated against the guest's own recorded disposition and mask:

* **blocked** (`sigmask_`, and the signal is maskable) → recorded in `signal_pending_`,
  syscall returns 0. `rt_sigprocmask` re-evaluates the pending set after every successful
  mask change and handles what it can now see — which is what makes `block(SIGUSR1);
  raise(SIGUSR1); unblock()` behave like Linux instead of killing the guest in the middle;
* **`SIG_IGN`**, or a default action that ignores for a single-threaded process
  (`SIGCHLD`, `SIGCONT`, `SIGURG`, `SIGWINCH`) → dropped, syscall returns 0. This was the
  most visible bug: a guest that ignores `SIGCHLD` and signals itself died;
* **stop class** (`SIGSTOP`/`SIGTSTP`/`SIGTTIN`/`SIGTTOU`) → dropped. There is no stopped
  state to enter, and terminating would end a run a real kernel would merely pause; the
  choice is documented rather than silent;
* **terminate class**, or a signal the mask cannot hold (`SIGKILL`, `SIGSTOP`) → the run ends
  with status `128 + signo`, which is what a caller observes;
* `signo == 0` → 0 (existence probe); `signo < 0 || signo > 64` → `-EINVAL`.

These are the outcomes a process without a scheduler and without a frame builder can
represent honestly. What is deliberately *not* here:

* **An installed handler is not invoked.** The run ends with `128+signo` instead. This
  needs, at minimum: the 248-byte `rt_sigframe` laid out exactly as x86-64 glibc expects
  (`pretcode`, `ucontext` with a full `sigcontext`, `siginfo_t`), the `SA_SIGINFO` vs
  `void(int)` calling convention, `SA_ONSTACK` (the recorded altstack), `SA_NODEFER` and
  `SA_RESETHAND` mask semantics, `SA_RESTART` syscall restart, and `rt_sigreturn` to undo
  all of it. Getting the frame wrong produces a guest crash that looks like an emulator
  bug, so it needs an end-to-end test with a real dynamically-linked guest process before it
  can be claimed. A test pins the current behaviour
  (`an_installed_handler_is_documented_as_not_invoked`) so the gap cannot quietly turn into
  a success return that drops the signal.
* **No asynchronous signals, by construction.** Nothing on the host side installs a handler,
  so there is no path by which a signal could interrupt generated code — which is also why
  the translator needs no safe-point machinery. The invariant is worth stating: signals are
  only raised at syscall boundaries, and a syscall already leaves generated code through the
  exit epilogue, so `GuestState` (including the pending PF/AF descriptor of §29.11) is
  coherent at every point where a signal can be decided. Host `SIGINT`/`SIGTERM` kill the
  emulator itself; a guest memory fault is reported through the fault path (`RunStatus::kFault`),
  not as a guest `SIGSEGV` with a frame.

#### Tests — eight new deterministic cases

`syscall.kill_ignores_an_ignore_class_default_signal`, `kill_honours_SIG_IGN`,
`kill_of_a_terminate_class_signal_ends_the_run`, `a_blocked_signal_is_held_and_delivered_when_unblocked`,
`an_ignored_blocked_signal_is_dropped_on_unblock`, `SIGKILL_cannot_be_blocked_or_ignored`,
`an_installed_handler_is_documented_as_not_invoked`,
`kill_rejects_a_signal_number_out_of_range`. Each drives `MinimalSyscallHandler` directly with
a real `GuestState` and `FlatMemory`, so they are deterministic and host-independent
(they run on both the native and the AArch64 builds — no `HYPERION_SKIP`).

#### Verification

* native (x86-64) clang-19: **349 passed / 0 failed / 47 skipped of 396**; `ctest` 19/19
* AArch64 (`qemu-aarch64-static`): **396 passed / 0 failed / 5 skipped of 401**
* no generated-code change: this item is entirely in the syscall layer, so the translator
  and its differential tests are untouched

### 29.17 P13 — threads: the supported model, defined and pinned; thread creation stays refused

Item 13 of §L asks what threading is supported and to implement it "incrementally". The
honest answer after the audit is that the supported model is **many processes, no threads**,
and that this is a design position rather than a missing afternoon of work: the emulator's
two tiers both assume a single thread owns the whole translation cache, the TLB, the
indirect-branch jump table and the guest address space.

#### What the supported model is, exactly

| property | supported behaviour |
| --- | --- |
| process creation | `fork`(57), `vfork`(58), `clone`(56) with an exit signal of `SIGCHLD` and only the flags `CLONE_CHILD_SETTID`, `CLONE_CHILD_CLEARTID`, `CLONE_PARENT_SETTID`, `CLONE_SETTLS`, `CLONE_UNTRACED`, `CLONE_VFORK` |
| the child | a **real host child** with a deep copy of the guest address space (`GuestAddressSpace` copy constructor) and its own `GuestState`; it runs the guest from the instruction after the `syscall` and exits the host process with the guest's status |
| address space | private per process. Nothing a child writes is visible to the parent, which is the POSIX meaning of `fork` |
| TLS | `%fs` base lives in `GuestState::fs_base` (set by `arch_prctl(ARCH_SET_FS)` or `CLONE_SETTLS`) and is therefore per process — again the correct meaning for processes |
| signal disposition and mask | recorded in `MinimalSyscallHandler`, i.e. per process |
| translation | the parent keeps its tier; a child runs tier 0 by default (`COMPATIBILITY.md`), so no translation state is ever shared between two live processes |
| `CLONE_CHILD_CLEARTID` | the child zeroes the tid word in **its own copy** on exit; a parent that polls the word deliberately does not see it (verified gap, see below) |
| thread creation | refused: `-ENOSYS` for any of `CLONE_VM`, `CLONE_THREAD`, `CLONE_FILES`, `CLONE_FS`, `CLONE_SIGHAND`, `CLONE_SYSVSEM`, `CLONE_PTRACE`, `CLONE_STOP`, `CLONE_IO`, and for every namespace flag; an unknown flag is refused rather than ignored |

Refusing is the safe behaviour, not a limitation to apologise for: a guest that asked for a
shared address space and received a private copy would corrupt memory in a way it cannot
detect. The refusal is pinned twice — end to end by a real x86-64 fixture
(`tests/guest/fixtures/raw_clone_flags.c`, run by `process_test.sh`) and now at unit level by
four new tests that also check the refusal **changes nothing**: `clone_thread_flags_are_refused_without_touching_guest_state`
(every GPR except `rax` and `rip` identical after the call), `clone_exit_signal_other_than_sigchld_is_refused`,
`clone_validates_tid_pointers_before_creating_a_child` (`-EFAULT` *before* any child exists) and
`clone_settls_with_a_null_tls_is_einval`.

#### What threads would actually require — and where the races would be

Written down because "just call pthread_create" is not the shape of the problem:

1. **Per-thread guest state**: `GuestState` (GPRs, `rip`, flags *and the pending PF/AF
   descriptor of §29.11*), `fs_base` (TLS), signal mask, and the alt-stack. Dispositions are
   shared under `CLONE_SIGHAND`; masks are not.
2. **Per-thread JIT context**: `ExecContext` holds the `SoftTlb` and its mapping epoch, the
   budget pointers, `jit_exit_epilogue` and the 1024-entry indirect-branch jump table. One
   thread's TLB invalidation must not corrupt another's; today there is exactly one of these,
   owned by the one `TranslationCache`.
3. **Shared translation state, i.e. the race surface**: the block map, the code arena, the
   direct-mapped block cache, and `lookup()` — which *writes* `ctx_->jump_table[jslot]` on
   every dispatch hit. Two threads dispatching simultaneously would race on that write today.
   The hot-block counters of §29.12 are a benign race by construction (a lost increment only
   delays a promotion), which is worth keeping in mind as the pattern for the rest.
4. **Code invalidation**: `invalidate_all()` runs when a syscall changes the guest's code
   epoch (self-modifying code, `mmap`/`mprotect` of executable pages). With threads it must
   stop every thread that could be executing a stale block — the classic translation-cache
   shootdown — or a thread will keep running superseded code.
5. **Synchronisation**: `futex` would need a real wait queue keyed by guest address with
   wake-one/wake-all and priority-inheritance refusal, plus `FUTEX_WAIT` blocking the *thread*
   rather than ending the run. Today the value comparison is real and `WAKE` honestly reports
   0 woken, because a single-threaded guest cannot have a waiter.
6. **Signals**: with threads there would be asynchronous delivery, per-thread masks, and a
   `killed by a signal` path that must interact with generated code — exactly the safe-point
   machinery §29.16 says does not exist because nothing can arrive asynchronously today.

That is a phase of work with its own risk class (locking in the hottest path, memory ordering
in the TLB, and a shootdown protocol), not an incremental change to be tacked onto this one.
Implementing a partial version — say, `CLONE_VM|CLONE_THREAD` accepted with a shared address
space but one execution context — would produce exactly the kind of unfounded POSIX claim that
this project refuses to make.

#### Verification

* native (x86-64) clang-19: **353 passed / 0 failed / 47 skipped of 400**; `ctest` 19/19
* AArch64 (`qemu-aarch64-static`): **400 passed / 0 failed / 5 skipped of 405**
* the four new tests are plain `syscall` tests: deterministic, no `HYPERION_SKIP`, and they
  create no host processes (the refusal and validation paths all return before the fork)

### 29.18 P14 — there is no IR today; here is the staged one, and the criterion for building it

Item 14 of §L proposes an incremental IR. It is the one item in this batch that is
deliberately *not* implemented, and the reason is in §29.14: an IR is a means to measurable
optimisations, and the two optimisations queued behind it (CF/ZF/SF/OF deferral, a return-target
cache) both have ceilings below this environment's noise floor. Building the IR now would be
structural churn with a claim attached that could not be verified.

#### What the translator is today

There is no IR. Translation is a three-pass pipeline over a decoded straight-line vector, twice
over (a legacy single-instruction path and the production cached path), plus a separate direct
switch interpreter:

| component | lines | role |
| --- | --- | --- |
| `src/core/decoder.cpp` | 2 262 | x86-64 → `Instruction` (opcode, operands, size, length) |
| `src/core/translator.cpp` | 3 788 | one-pass AArch64 emission, with block-level register allocation and flag liveness in front of it |
| `src/core/interpreter.cpp` | 3 150 | the reference semantics; every optimisation is validated against it |

`translate_cached_block` is: **pass 1** decode up to the terminator (`Step { Instruction,
live_flags }`), **pass 1.5** per-block register allocation (pin a guest register in `x19..x23`
when a block references it more than twice), **pass 2** exact backward flag liveness across the
ISA, **pass 3** emit — and emission is the only place that knows the machine-level details, so
every fact an optimisation needs is re-derived at the point of use.

ISA knowledge is therefore spread across predicates that ~28 call sites consult independently:
`flags_written`, `flags_written_full`, `flags_read_full`, `flags_killed_full`,
`is_classic_fast_alu`, `condition_flags`, `try_nzcv_cond`, and the `emit_one` switch itself.
That is exactly the shape a `cc_op` descriptor must *not* be added to: §29.14's list of
producers and consumers would have to be reproduced at ~30 emitter sites.

#### The staged proposal

| stage | content | acceptance criterion |
| --- | --- | --- |
| 0 (implicit, exists) | `std::vector<Step>` + flag liveness is already a mini-IR | — |
| **1** | `IrBlock`/`IrInsn`: the decoded instruction plus, computed *once*, the facts every later pass needs — flags read / written / killed, GPR read and write masks, `writes_memory`/`reads_memory`, terminator kind, fault-PC and step-weight. `translate_cached_block` builds it in the existing pass 1 and keeps registering it into the allocator and liveness passes | **generated code is byte-for-byte identical** on a corpus of blocks (compare `total_code_bytes()` and the emitted bytes), all tests green, no timing change beyond noise. A pure refactor that cannot be wrong in a way the tests do not see. |
| **2** | first consumer: flag-materialisation sinking/DSE inside a block, driven by the facts instead of by per-site guesses (this is what §29.14's stage 2 needs, and it is worth testing on its own before any six-flag descriptor) | differential parity with the interpreter on the full ISA corpus **and** a measured win above the noise floor (§29.14 sets that bar); the current emitter stays as a fallback until both hold |
| **3** | effective-address folding and a branch representation that can carry an indirect target cache (§29.13's deferred ret/`jmp r/m` optimisation), which needs an IR-level notion of a branch rather than a code-offset patch list | differential parity plus a measured win |
| **4** | retire the legacy `translate_block`/`translate_instruction` paths once the cached path is the only one exercised, so there is one emitter and one reference | the tests that reach the legacy path by name are re-pointed at the cached path, not deleted |

Invariants any IR pass must preserve, because they are what makes the current design verifiable:

* **one `Instruction` = one retired guest instruction** in the budget accounting, including
  instructions that fault (the step is un-retired on the fault path);
* **faults report the faulting instruction's own pc**, not the block's;
* **memory ordering and the `SoftTlb` epoch checks stay in emission**, not in the IR: an IR pass
  may not reorder or elide guest loads/stores across a fault point;
* **the pending PF/AF descriptor (§29.11) stays valid at every boundary** — helper entry, block
  exit, consumer guard — because it is C++-visible state, so a pass that moves or removes a
  store must re-derive it rather than assume it;
* **direct-link and call-link offsets (`exit_links`, `code_offset`) are emitted-code offsets**,
  so a pass that changes code layout must keep them consistent — which is another reason stage 1
  is defined as "no output change".

#### Decision

Proposal recorded, stages 1–4 not implemented here, because the item's own acceptance criterion
is "measurable value" and the two optimisations behind it are both under the measurement floor
of this setup (§29.13: a ret-target cache is worth ~4–5 % on call-heavy code only; §29.14: the
remaining flags are ~4 %). The work is not rejected — it is sequenced: it becomes worthwhile the
moment either a native-AArch64 measurement rig with ≤1 % noise exists, or stage 2 can be justified
by a specific pass whose own measurement clears the bar.

### 29.19 P5 re-validated on current main (option A), with the abort/regression case found

P5 shipped in `b0583e8` (§29.6): one write window per dispatcher miss, the entry block's
guessed successors translated inside it, one `publish()` for the window, links patched while
the page is still writable. It has now been re-measured on top of P6 (hot-block gating) and
P10 (call linking), because both change which blocks get translated and when.

#### Counters — P5 on vs off, same binary, same guests

A/B build differs by exactly one `getenv` early-return in `prefetch_successors` (temporary,
never committed). Guests are the repository's own x86-64 workloads (`benchmarks/workloads/`,
static, `-O2`), run through the AArch64 JIT under `qemu-aarch64-static`.

| guest | blocks (on / off) | `mprotect` calls (on / off) | per block on | per block off | **reduction** |
| --- | --- | --- | --- | --- | --- |
| `sieve` | 588 / 374 | **173** / 374 | 0.294 | 1.00 | 6.5× vs pre-P5 2.00 |
| `nqueens` | 525 / 355 | **163** / 355 | 0.310 | 1.00 | 6.9× |
| `quicksort_c` | 631 / 411 | **194** / 411 | 0.307 | 1.00 | 6.9× |
| `qsort_glibc` | 653 / 413 | **196** / 413 | 0.300 | 1.00 | 6.9× |

("off" is 1.0 rather than the historical 2.0 per block because the write-window plumbing
remains; switching only the speculation off returns to one publish per block, i.e. half of the
original two `mprotect`s. The pre-P5 figure of 2.00 — one RW, one RX per `allocate()` — is the
one in §29.6 and is what these rows are compared against.)

Secondary effect, and a larger one than expected: **linked exits rise by 5.8×** (sieve 100 → 576,
nqueens 92 → 516, quicksort_c 114 → 623, qsort_glibc 111 → 639). A successor that is translated
inside its predecessor's window can be linked immediately, because §29.7's patch is only legal
while the source page is still writable — the speculation is what makes direct linking succeed on
the first attempt instead of after a second window.

Cost side: 57 % more blocks exist than are executed for these guests (588 vs 374 on sieve), all
inside the 16-block / 3584-byte budget.

#### Wall clock — interleaved pairs, 14 samples each after discarding the first

| guest | P5 off (median / min / IQR) | P5 on (median / min / IQR) | on/off | paired-ratio median |
| --- | --- | --- | --- | --- |
| `sieve` | 280.2 / 268.5 / 270–307 ms | 288.5 / 265.1 / 271–332 ms | **1.029** | 1.021 |
| `nqueens` | 517.7 / 485.6 / 494–535 ms | **415.9** / 373.7 / 382–431 ms | 0.803 | 0.784 |
| `quicksort_c` | 377.6 / 355.9 / 366–395 ms | **305.9** / 298.9 / 303–319 ms | 0.810 | 0.817 |
| `qsort_glibc` | 492.7 / 467.3 / 477–505 ms | **441.9** / 425.6 / 431–455 ms | 0.897 | 0.895 |

So the write window is worth **10–20 % of whole-process wall clock** on three of the four
workloads, and `sieve` measured ~3 % *slower* (median), with overlapping IQRs.

**That sweep was re-measured with a better rig in §29.20 and the sieve "regression" is not there:**
timing only the execution loop, the paired ratio is 1.013 with a 95 % CI of [0.961, 1.055] — no
significant change. The earlier 3 % was process-start noise (qemu load plus ELF relocation), which
is exactly what §29.20 exists to remove. The number below is left in place, flagged, rather than
deleted, because "a 3 % regression that was really 0 %" is the useful lesson about this host.

#### Verification on current main

* targeted: `exec_mem.*` 9/9 (including `arena_write_window_defers_one_mprotect_per_window`,
  `arena_patches_code_inside_a_window_and_refuses_it_afterwards`,
  `arena_published_blocks_are_executable_and_reset_rearms_the_slab`) and `translation_cache.*`
  27/27 under qemu
* full native: 353 passed / 0 failed / 47 skipped of 400; `ctest` 19/19
* full AArch64: 400 passed / 0 failed / 5 skipped of 405
* generated-code counters unchanged from §29.12/§29.13 for the same guests; no test assertion
  needed updating, because P5 was landed with its exact counts already pinned

W^X is unchanged: a page is never writable and executable at the same time, and no alias mapping
exists (option C stays rejected).

### 29.20 A measurement rig that can see single-digit changes (`--timing`)

Everything deferred so far (P8's 6 %, P9 stage 2's ~4 %, P10's ret-target cache at ~4–5 %) was
deferred because whole-process wall clock on this host has a spread of 12–30 %: qemu load alone is
~45 ms and ELF relocation another ~10 ms, against run times of 0.3–1 s, and none of that has
anything to do with the code being measured. A benchmark that cannot see a 4 % effect cannot
justify — or refute — a 4 % change, so the measurement gap was the real blocker, not the ideas.

`hyperion run --timing` now reports the **execution loop's own wall time** on stderr:

```
hyperion: run wall 343.873 ms (8.734 ns/guest-instruction) 39373499 steps
```

* measured with `steady_clock` around `run_program()` / `TranslationCache::run()` only — process
  start, ELF load, relocation, the syscall registry and all reporting are outside it;
* written to **stderr**, so a guest's stdout, its step count and its exit status are unchanged
  (pinned by `tests/cli/cli_smoke.sh`, native and under qemu);
* works for both entry points (ELF and raw blob), because a raw blob is a first-class workload.

#### Resolution, measured rather than assumed

Re-running the P5 A/B of §29.19 with this metric, 14 interleaved pairs per guest after discarding
the first, bootstrap 95 % CI on the median ratio (`P5 on / P5 off`):

| guest | P5 off | P5 on | ratio | 95 % CI | verdict |
| --- | --- | --- | --- | --- | --- |
| `sieve` | 229.3 ms | 231.9 ms | 1.013 | [0.961, 1.055] | **no significant change** |
| `nqueens` | 461.6 ms | 338.6 ms | 0.739 | [0.698, 0.815] | P5 faster |
| `quicksort_c` | 309.8 ms | 256.8 ms | 0.833 | [0.804, 0.862] | P5 faster |
| `qsort_glibc` | 434.2 ms | 390.9 ms | 0.906 | [0.897, 0.915] | P5 faster |

CI half-widths are 1–4 %, so a 4–6 % effect is now decidable, and the three "P5 faster" verdicts
are correspondingly stronger than the whole-process numbers (26 %, 17 %, 9 % versus 20 %, 19 %,
10 %): with startup removed, the write window's benefit is measured on the code it actually
affects.

The independent in-process harness (`benchmarks/hyperion_tier_bench`, which times repeated samples
inside one process) reports an IQR fraction of 2.2 % for tier 1 and 1.2 % for tier 0, which is the
floor available on this host; `--timing` sits close to it because it removes the same startup bias,
at the cost of one run per sample.

#### Consequences for the deferred items

* **P8 (cross-block register allocation)** now has a rig that can resolve its ~3–4 % realistic
  ceiling. It is still gated behind a design that touches the guest-state-visibility invariant, so
  it stays deferred, but it is now testable rather than unfalsifiable.
* **P9 stage 2 (CF/ZF/SF/OF)** likewise: 4 % is above this rig's half-width, so the item is no
  longer *unmeasurable* — it remains unimplemented because the emitter surface (≈30 sites, §29.14)
  is a rewrite that should follow the P14 flag-effect table rather than precede it.
* **P10's ret-target cache** (~4–5 % on call-heavy code) is now decidable, and it is the next
  performance item to attempt, on this rig.

Nothing in this section changes generated code; it changes what can be claimed about it.

### 29.21 P10b (the deferred ret-target cache) — measured upper bound, and rejected

§29.13 measured the call side of the call/return path (worth implementing: direct calls are now
linked) and deferred the `ret` side to "a verified return-target cache", estimating ~4–5 % of a
call/return-heavy guest from a guest where CALL/RET were 20.4 % of retired instructions. That
guest was synthetic. With the rig from §29.20 it is now cheap to ask how representative it was,
by counting opcodes on the repository's own workloads (temporary tier-0 histogram build, never
committed):

| guest | total guest insns | `call` | `ret` | `jcc` | `jmp` | largest single opcode |
| --- | --- | --- | --- | --- | --- | --- |
| `nqueens` (recursive) | 34 235 010 | 2.51 % | 2.51 % | 7.54 % | 0.02 % | `mov` 27.4 % |
| `quicksort_c` | 21 627 270 | 0.06 % | 0.06 % | 17.17 % | 0.80 % | `mov` 27.6 % |
| `sieve` | 16 999 369 | 0.01 % | 0.01 % | 24.41 % | 0.04 % | `mov` 31.1 % |
| `qsort_glibc` | 20 489 326 | 3.87 % | 3.87 % | 10.66 % | 0.98 % | `mov` 29.2 % |

Even on the most call-heavy real guest, `ret` is 3.9 % of retired instructions. A cached
return target would replace the shared dispatcher's 11 instructions with roughly 5 (index, compare,
branch), i.e. save ~6 host instructions per ret — about **0.23 host instructions per guest
instruction, or ~1–2 % of runtime** on that guest, and under 1 % everywhere else. That is at the
edge of resolvability (§29.20's half-widths are 1–4 %) and, more importantly, it is *not* the
4–5 % §29.13 extrapolated.

Decision: **rejected**, with the numeric reason recorded rather than a hunch. The estimate in
§29.13 is corrected here, and the design is still written down there in case a guest with a
genuinely different call/ret profile appears (deeply recursive interpreters, for instance, where
call/ret can exceed 15 % — that would move it back into range).

The same histogram points somewhere else, and that is the more useful output of this measurement:
**`mov` alone is 27–31 % of retired instructions** on every one of these guests, with
`cmp`/`jcc`/`add` behind it, while `call`/`ret` never exceed 3.9 %. Any further work on this
translator's per-instruction cost should start from the move path (a register-to-register move
that materialises through `GuestState` where an allocated host register would do, or a block-level
copy-propagation pass) rather than from the call/return path. That belongs to the P14 stages of
§29.18, where an IR pass can see a whole block's moves at once.

### 29.22 Sweeping the register-allocation threshold — flat, so the heuristic stays

§29.15 measured cross-block register traffic (6.1–6.6 % of generated code) and §29.21 pointed at
`mov` as the single largest opcode class (27–31 % of retired instructions). The cheapest hypothesis
that connects the two is that the allocator's entry condition is mistuned: it pins a guest register
only when a block references it more than twice (`refs[i] > 2`, justified as "strictly saves ≥ 1
word over 1 load + 1 store"), and at ~4.7 guest instructions per block that fires for 0.65–0.75
registers per block. If `mov` dominates, a looser condition would keep more operands in host
registers and remove GuestState traffic.

So it was swept, with a temporary `HYPERION_RA_MIN_REFS` hook in the allocator (never committed),
on the §29.20 rig: 11 interleaved samples per configuration, bootstrap 95 % CI on the median ratio
against today's `> 2`:

| threshold | `nqueens` | `quicksort_c` | `sieve` | `qsort_glibc` |
| --- | --- | --- | --- | --- |
| `> 1` (pin at 2 refs) | 0.994 [0.938, 1.012] | 1.003 [0.926, 1.045] | 1.005 [0.927, 1.067] | 1.003 [0.982, 1.019] |
| `> 2` (current) | — | — | — | — |
| `> 3` | 1.016 [0.955, 1.064] | 0.984 [0.967, 1.055] | 1.020 [0.944, 1.085] | 0.993 [0.942, 1.016] |
| `> 4` | 1.038 [0.913, 1.129] | 1.012 [0.935, 1.035] | 1.006 [0.904, 1.036] | 1.025 [0.978, 1.129] |

**Every configuration is inside the noise: no threshold is measurably better or worse than the
current one**, on any of the four workloads. The generated-code sizes do move as expected
(pinning at 2 refs costs 2.3 % more code, 240 804 B vs 235 388 B for `quicksort_c`), so the
experiment really did change the allocation — the *runtime* simply does not care, which says the
GuestState round-trip for a twice-referenced register is already about as cheap as the two
instructions it would replace.

Consequences, recorded rather than acted on:

* the current heuristic stays, now with a measurement behind the constant instead of only an
  instruction-count argument;
* it corroborates §29.15's conclusion that a cross-block allocator (P8) has little to win: if
  moving *within* a block between "in a host register" and "in GuestState" is worth ~0, moving
  *across* blocks is worth less than the 6 % of code those transitions occupy;
* and it means `mov`'s 27–31 % share cannot be attacked by allocation policy at all. It needs a
  pass that sees a block as a whole and rewrites the moves away — block-level copy propagation,
  which is a P14 stage-2 pass (§29.18), not a tuning constant.

### 29.23 P14 stage 1 implemented — a facts pass that provably changes no output

§29.18 defined stage 1 as "the decoded instruction plus, computed *once*, the facts every later
pass needs", with the acceptance criterion *generated code byte-for-byte identical*. Both are now
in place on `main`.

#### What exists

`include/hyperion/translator.hpp` carries the whole public surface:

```cpp
struct IrFacts {
    std::uint8_t  flags_read, flags_written, flags_killed;   // ir_flags::{kCf,kPf,kAf,kZf,kSf,kOf}
    std::uint16_t gpr_reads, gpr_writes;                     // bit i == Gpr(i)
    bool          reads_memory, writes_memory;
    bool          is_terminator;
    std::uint8_t  step_weight;                               // retired instructions, always 1
};
struct IrInsn { Instruction insn; IrFacts facts; std::uint8_t live_flags; };
IrFacts compute_ir_facts(const Instruction&);
```

`compute_ir_facts` is a pure function of the decoded `Instruction` alone. Inside
`src/core/translator.cpp` the same computation lives in `compute_facts` in the file's anonymous
namespace, because it must call the existing flag/register helpers
(`flags_read_full`, `flags_written_full`, `flags_killed_full`, `regs_read_write_full`) directly;
the exported wrapper simply forwards. `translate_cached_block`'s `std::vector<Step>` became
`std::vector<IrInsn>` and pass 1 now fills `facts` as it decodes, so pass 1.5 (register
allocation) and pass 2 (flag liveness) read fields instead of calling the helpers again. No helper
was rewritten, no predicate was changed, no emission site was touched — which is why the output
cannot move.

#### Validity and invalidation, stated as rules

The facts describe **one decoded instruction**, not a program point:

* **Validity.** `facts` is valid from the moment pass 1 constructs the `IrInsn` until the
  instruction is re-decoded. It is a fact about the encoding, so re-decoding the same bytes always
  yields the same facts (a test pins this).
* **Invalidation.** Nothing invalidates a fact except replacing the instruction itself. A pass may
  not "update" `facts` incrementally; if it rewrites `insn` (a later stage that folds an immediate
  into a displacement, say) it must call `compute_ir_facts` again on the new instruction. The
  struct carries no state that a *previous* instruction could have contributed, and no pass may
  add any: an `IrInsn` that depends on its neighbours is no longer block-local.
* **`flags_killed` vs `flags_written`.** `written` is "the instruction produces a value for this
  flag"; `killed` is the strictly weaker "any previous value for this flag is dead". They differ
  for the deferred-PF/AF path (§29.11), where the descriptor stays pending across the instruction;
  the invariant `killed ⊆ written` is asserted by the unit tests, and a consumer that needs
  "definitely dead" must use `killed`, never `written`.
* **`live_flags` is *not* a fact.** It is the pass-2 liveness solution for that index, filled in
  after pass 1. It is the one field whose value depends on position in the block, so any future
  stage that inserts, deletes or moves an `IrInsn` must re-run pass 2. Keeping it inside the same
  struct is a data-layout convenience, not a claim of block-local validity.
* **Memory facts are conservative by construction.** `reads_memory` is set for any memory operand
  (an addressing form can always fault), `writes_memory` only for opcodes that really store, and
  both are forced on where the truth is dynamic — string operations (direction from DF) and the
  implicit stack traffic of `push`/`pop`/`call`/`ret`/`leave`. A consumer may use `reads_memory`
  to *prevent* movement; it may never use its absence to justify movement of anything else.

#### The deliberate gap, pinned by a test

`regs_read_write_full` describes *explicit* operands (plus `rsp`/`rbp` where the allocator needs
them). `mul`, `div`, `idiv`, `cqo`, and the implicit string operands fall through its `default:
break` and report **no** registers at all. That is pre-existing behaviour, it is safe today
because those opcodes execute through a helper that reloads `GuestState`, and stage 1 must not
"fix" it: adding the implicit RAX:RDX pair would change pass-1.5 refcounts and therefore the
generated code, breaking byte identity. `ir_facts.mul_reports_its_flags_but_not_its_implicit_registers`
asserts `gpr_reads == gpr_writes == 0` for `mul rbx` so the boundary cannot be forgotten, and any
future pass that reads the masks must treat those opcodes as opaque.

#### Tests

`tests/unit/test_ir_facts.cpp`, 17 cases: `mov` reg/reg and immediate; byte and word partial
writes; high-byte aliases (`84 FC`); `cmp`; `inc`/`dec` preserving CF; `mul` (flags and the gap
above); `adc` consuming CF; the condition-code reads of `je`/`jb`/`jbe`/`seto`; a store versus a
`cmp`/`test` load; SIB index registers counted as reads; the implicit stack traffic of
`push`/`pop`/`call`/`ret`/`leave`; terminator classification; conservative `rep movsb` bits;
purity/reproducibility; `killed ⊆ written`.

#### Byte-identity evidence

Two trees, both built with the *same* temporary instrumented `translation_cache.cpp` that hashes
every translated block's emitted bytes (an ad-hoc FNV-style digest printed from a destructor when
`HYPERION_CODE_DIGEST` is set — never committed): `070168d` from `git archive`, and the working
tree. Guests are static `gcc -O2` binaries, run under `qemu-aarch64-static`. The environment is
pinned with `env -i PATH=/usr/bin:/bin` because the guest inherits the host environment and static
glibc's startup takes different paths depending on it — which changes *which* blocks execute at
all (sieve translates 494 blocks with the ambient environment, 509 under `env -i`).

| guest | blocks | bytes | digest (baseline **and** stage 1, 2 runs each) |
| --- | --- | --- | --- |
| sieve | 509 | 179 696 | `ef9e708d80ef5c8a` |
| nqueens | 425 | 143 232 | `0076e919dff74537` |
| quicksort_c | 557 | 192 824 | `3f8274d269707116` |
| qsort_glibc | 581 | 218 908 | `b7957322d9fa1ed3` |

Identical for all four guests, so stage 1 is a pure refactor in the strong sense: the machine code
is bit-for-bit the same, not merely equivalent.

#### Verification

| configuration | result |
| --- | --- |
| native (clang 19, RelWithDebInfo) | 370 passed, 0 failed, 47 skipped (was 353/0/47) |
| `ctest` native | 19/19 |
| AArch64 cross, unit tests under `qemu-aarch64-static` | 417 passed, 0 failed, 5 skipped (was 400/0/5) |
| `cli_smoke` | green on both trees |

Known platform limitation, unchanged: the three `*.rejects_single_sample` bench tests invoke the
benchmark binaries directly, so in the cross tree they try to execute AArch64 ELF natively and fail
with a shell syntax error. They are unrelated to this change and fail identically without it; the
AArch64 gate is the unit-test binary under qemu.

### 29.24 P14 stage 2 (block-level copy propagation) — measured, and rejected: the mov mass is not where a copy pass can reach it

Stage 1 (§29.23) delivered the facts pass. Stage 2 was to be copy propagation, the first consumer.
It is **not implemented**, because the measurement that was supposed to justify it says the
removable fraction is ~0.05 % of translated instructions — two orders of magnitude under this
rig's noise floor (§29.20), and below the 1–2 % at which a `--timing` A/B can even be read.

The instruction that matters here was explicit: *do not treat the 25–31 % `mov` share as a 25–31 %
opportunity; measure the removable fraction first*. That measurement is below.

#### How it was measured

A temporary hook in `translate_cached_block` (never committed) dumped, for every block the four
benchmark guests actually translate, the decoded instruction stream plus the facts stage 1
computes: operand kinds and widths, high-byte flag, memory base/index/displacement, the four flag
masks, the GPR read/write masks, and the per-instruction live-flags solution, together with the
guest registers pass 1.5 allocated for that block. An offline analyser then applied the actual
codegen cost model of this translator to each block:

* an **unallocated** guest register costs one `ldr` per read and one `str` per write; an allocated
  one costs nothing (its prologue/epilogue is 2 words per block, already paid);
* a `mov r, r` with neither operand allocated is therefore 2 words, with one allocated 1 word, and
  both allocated a single `mov`;
* a **copy is removable outright** only if its destination is fully overwritten (`mov`/`movzx`/
  `movsx`/32-bit write) *before any later read of it in the block*, and its source is not memory
  (a dead load can still fault, so dead loads are never removed);
* a copy can **also** be removed by renaming its destination to its source for the rest of the
  block — but only if the destination is dead at block exit, which is almost never true here:
  `GuestState` is authoritative at every boundary again, so every guest register is live-out and
  the destination's store must happen somewhere. Renaming therefore saves only *reloads of an
  unallocated destination that alias an allocated source*.

Guests: `sieve`, `nqueens`, `quicksort_c`, `qsort_glibc` (static `gcc -O2`), translated under
`qemu-aarch64-static` with the environment pinned (`env -i PATH=/usr/bin:/bin`).

#### The static corpus

| guest | blocks | insns | insns/block | `mov` | share | store r→[m] | load [m]→r | `mov r,imm` | copy r←r 64 | copy r←r 32 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sieve | 501 | 2 309 | 4.6 | 631 | 27.3 % | 178 | 194 | 44 | 160 | 53 |
| nqueens | 425 | 1 927 | 4.5 | 490 | 25.4 % | 129 | 148 | 35 | 129 | 47 |
| quicksort_c | 549 | 2 535 | 4.6 | 696 | 27.5 % | 190 | 210 | 44 | 181 | 69 |
| qsort_glibc | 572 | 2 710 | 4.7 | 727 | 26.8 % | 192 | 237 | 51 | 198 | 47 |

The `mov` share reproduces the earlier dynamic figure (25–31 %), which is the point: the share is
real, and it is **60 % memory-operand moves** (372–429 of 490–727), not register copies. A copy
propagation pass cannot touch a `mov [rbp-0x18], rdi` — that is a guest store, not an alias.

#### What block-local copy propagation could actually reach

| guest | copies with dst read again (used) | dst never read again (block-final) | dst fully overwritten first (**removable**) | reads that renaming could reach | words renaming saves | removable % of insns | words saved |
| --- | --- | --- | --- | --- | --- | --- | --- |
| sieve | 32 | 127 | **1** | 55 | 0 | 0.043 % | 1 |
| nqueens | 27 | 101 | **1** | 52 | 0 | 0.052 % | 1 |
| quicksort_c | 37 | 142 | **2** | 72 | 0 | 0.079 % | 2 |
| qsort_glibc | 41 | 155 | **2** | 62 | 0 | 0.074 % | 2 |

Two numbers carry the decision:

1. **At most 2 instructions per guest** are removable outright (0.04–0.08 % of all translated
   instructions), worth **1–2 emitted words out of ~4 800–6 800** — 0.03 %. Not a fraction that
   can be measured, let alone claimed.
2. **Renaming saves exactly zero words**, in every guest. There is not a single case where an
   unallocated copy destination is re-read while its source sits in an allocated host register:
   when a register is referenced more than twice it is already pinned by pass 1.5, so the alias
   target is allocated too, and the reload it would have saved was already free.

#### Why the structure makes this the expected answer

Blocks average **4.5–4.7 guest instructions**. Of the 64-bit copies, **79–83 % have a destination
that is never read again inside its own block** — they are call-argument setup (`mov rdi, rax`
immediately before a `call`) and return-value moves (`mov rax, …` immediately before a `ret`),
which is exactly where a block ends. Copy propagation needs a *use* after the copy, inside the
same block, to have anything to propagate into; with a mean of 4.6 instructions per block and a
`jcc` roughly every seven, those uses are in the next block almost every time.

This is the same wall §29.15 (P8) hit from the other side, where the measurement was "there is
almost nothing worth keeping across a block boundary", and §29.22, where loosening the
register-allocation threshold was flat because the allocator already takes every register with
more than two references. Three independent measurements now agree, from three directions, that
the block is the wrong granularity for value-level optimisation *and* that the current
per-block allocation already captures what is there.

Two further checks were run so the rejection is not an artefact of one definition:

* **32-bit copies** (`mov eax, ebx`, which zero-extends and so is not a plain alias) were counted
  separately — 47–69 per guest, all block-final or used-with-zero-saving, same conclusion.
* **Store→load forwarding** for the memory-form moves — the one other block-local redundancy
  elimination the corpus suggests — was measured too: of 77–104 plain `[base+disp]` 64-bit stores
  per guest, **zero** are followed in the same block by a load of the same slot before any
  intervening store, call or base-register write.

#### Decision

Stage 2 as specified — block-level copy propagation over the stage-1 facts — is **rejected**, and
the reason is a measurement rather than an effort estimate: it can remove at most 0.05 % of
translated instructions and zero emitted words on the corpus this project measures, i.e. its
effect is unobservable with the rig that would have to certify it. Implementing it would add an
alias table, width and partial-write tracking, invalidation rules and a new class of bugs to the
translator, in exchange for code-size noise. That fails the criterion the item set for itself
("measurable value", §29.18), so nothing is implemented and no fallback switch is added.

What would change the verdict, stated so the decision can be revisited rather than re-argued:

* **A block granularity that puts a copy and its use in the same block** — a trace/superblock
  builder, or block extension across unconditional jumps and through `jcc` fall-through. With
  mean block length 4.6, doubling or tripling reachable block length is the precondition for
  every value-level pass, this one included, and it is a *control-flow* change (§29.18 stage 3),
  not a dataflow change.
* **Cross-block liveness proven on the successor set** (§29.15 concluded it is worth ~nothing,
  but that measurement predates the facts pass and assumes the same short blocks).
* Or the cheaper, orthogonal direction the corpus does point at: 60 % of the `mov` mass and the
  bulk of the per-instruction cost are *guest-state memory accesses* (0.68–0.71 per instruction
  survive allocation). Reducing those means either more pinned host registers — blocked by the
  `x19..x23` window that `enter_jit` saves, with `x24..x28` still in use for dispatch, budget and
  state — or folding the load/store of a value that an adjacent instruction has just produced,
  which is a *scheduling* pass over ≥2 instructions and again needs the longer block.

Stage 1 stays: the facts are the correct substrate for whichever of those comes first, and stage 1
cost nothing (§29.23's byte-identity result). This section closes item 14 with stages 1 shipped and
stage 2 rejected on evidence.

### 29.25 P14 stage 1 verified end to end: A/B timing, code size, and the one cost it does add

Stage 1 (§29.23) is a refactor, so the only honest claims are *no change* claims, and they are
checked here against a clean `070168d` build (`git archive 070168d`, same toolchain, same
`RelWithDebInfo` cross configuration, tests and benchmarks off, no instrumentation) on the four
benchmark guests.

#### Generated code: identical, measured two independent ways

| guest | `070168d` | `24f6477` (stage 1) |
| --- | --- | --- |
| sieve | 171 712 B / 2 274 guest insns | 171 712 B / 2 274 |
| nqueens | 141 692 B / 1 895 | 141 692 B / 1 895 |
| quicksort_c | 184 460 B / 2 495 | 184 460 B / 2 495 |
| qsort_glibc | 211 168 B / 2 683 | 211 168 B / 2 683 |

`total_code_bytes()` and the translated-instruction counts are equal for all four guests — the
same numbers the digest hook produced byte for byte in §29.23, now from an uninstrumented pair of
binaries. Guest output is also unchanged: the md5 of each guest's stdout is identical under
`070168d`, under stage 1, and natively on the host, and the `--timing` step counts match exactly
(16 990 960 / 39 363 637 / 21 673 085 / 21 606 988).

#### Run time: two interleaved A/B sessions, 12 pairs per guest each

`--timing`'s in-process run wall (execution loop only), A and B alternating within each pair so
machine drift hits both sides, environment pinned with `env -i PATH=/usr/bin:/bin`.

| guest | session | baseline median (ms) | stage 1 median (ms) | paired-ratio median | 95 % CI |
| --- | --- | --- | --- | --- | --- |
| sieve | 1 | 199.51 | 199.61 | 1.016 | [0.980, 1.061] |
| sieve | 2 | 202.50 | 195.58 | 0.994 | [0.890, 1.016] |
| nqueens | 1 | 284.59 | 287.42 | 0.983 | [0.963, 1.010] |
| nqueens | 2 | 281.00 | 280.57 | 1.003 | [0.958, 1.053] |
| quicksort_c | 1 | 219.24 | 221.40 | 1.017 | [0.973, 1.039] |
| quicksort_c | 2 | 217.18 | 217.46 | 0.985 | [0.967, 1.021] |
| qsort_glibc | 1 | 327.92 | 325.88 | 1.006 | [0.961, 1.030] |
| qsort_glibc | 2 | 336.97 | 339.36 | 1.020 | [0.981, 1.032] |

Every CI straddles 1.0 in both sessions, which is the acceptance criterion: a refactor that cannot
change the machine code cannot change the run, and the rig (§29.20) is sensitive enough to say so
rather than merely failing to detect a change.

#### The cost that is real: translation time

`translation_ns()`, 15 interleaved pairs per guest:

| guest | baseline (ms) | stage 1 (ms) | ratio | 95 % CI |
| --- | --- | --- | --- | --- |
| sieve | 40.90 | 43.52 | 1.037 | [0.990, 1.109] |
| nqueens | 38.40 | 40.33 | 1.022 | [0.989, 1.103] |
| quicksort_c | 43.18 | 44.61 | 1.023 | [0.987, 1.065] |
| qsort_glibc | 46.27 | 47.15 | 1.018 | [1.000, 1.061] |

Medians are 1.8–3.7 % above baseline in all four guests and three of the four CIs touch or exclude
1.0 — the direction is consistent, so it is reported rather than dismissed. It is *not* acted on,
for three reasons, and the reasoning is recorded so a future change can revisit it:

* Translation is **15–20 % of run wall** on these guests (42–48 ms of 216–339 ms), so 2–4 % of it
  is 0.3–0.7 % of the run — below the resolution of the run-time rig that Stage 1 is judged by,
  and below the level at which any of the optimisations in this batch were accepted.
* The mechanism is understood: the facts are computed eagerly for every decoded instruction,
  inside the decode loop, instead of lazily by the two passes that need them, and `IrInsn` is
  168 bytes against `Step`'s 160. Neither is free, neither is large, and reversing them means
  either splitting the computation back out of pass 1 (re-reading the instruction stream twice,
  which is what stage 1 was meant to avoid) or packing `IrFacts` and `live_flags` into the 8 bytes
  of padding after `Instruction` — which does not fit: the three flag bytes, two register masks
  and a live-flags byte are already 9.
* These numbers are measured **through `qemu-aarch64-static`**, i.e. on emulated AArch64, where
  extra memory traffic and extra C++ work are amplified relative to a real AArch64 host. Chasing
  a 0.3–0.7 % run-time-sized effect with an emulator-mediated measurement would be guessing.

What would make it worth doing: a translator-only workload where the miss path dominates (short
guests, `hyperion_translate_bench`), or a real AArch64 measurement host.

#### Test matrix at `24f6477`

| configuration | result |
| --- | --- |
| native x86-64, clang 19, `RelWithDebInfo` | 370 passed / 0 failed / 47 skipped of 417 |
| `ctest` (native) | 19/19 |
| AArch64 cross, unit tests under `qemu-aarch64-static` | 417 passed / 0 failed / 5 skipped of 422 |
| `cli_smoke.sh` native | pass (static and dynamic PT_INTERP ELF) |
| `cli_smoke.sh` AArch64 under the emulator | pass (same digests as native) |
| GCC 14 strict (`HYPERION_WARNINGS_AS_ERRORS=ON`) | builds clean, 370 passed / 0 failed |
| clang 19 strict (`HYPERION_WARNINGS_AS_ERRORS=ON`) | builds clean, 370 passed / 0 failed |
| guest stdout vs native x86 execution | identical for all four guests |
| `--timing` step counts | identical to baseline for all four guests |
| GitHub Actions, all three pushes (`24f6477`, `9ee9cff`, `e393f0a`) | 7/7 checks green: build & test (gcc), build & test (clang), ASan/UBSan, AArch64 cross tests under qemu-user, clang-format, Android syscall policy paths, syscall inventory & shellcheck |

Known platform limitation, unchanged and unrelated to this work: the three
`*.rejects_single_sample` bench tests in `benchmarks/CMakeLists.txt` exec the benchmark binaries
directly, so in the cross tree they try to run AArch64 ELF on the x86 host and fail with a shell
syntax error. They fail identically at `070168d`; the AArch64 gate for this project is the unit
test binary under `qemu-aarch64-static`.

### 29.26 P15 — trace/superblock formation: investigated, designed, and not implemented — the hot paths are already linked, and what is left is shorter than the noise floor

P14's rejection (§29.24) ended by naming the one change that could give value-level passes
something to work on: longer translation units. This section is that investigation. It was run as
a separate item (P15) with the phases in order — restore and baseline first, measurements second,
design third, success criteria fourth, implementation only if the evidence supported it. It does
not. No production code changed; the only code written was a temporary probe in the tier-0
interpreter (described below), removed before this was committed, with the tree verified clean
and both builds re-run green afterwards.

#### Phase 0: restore, and a baseline that is actually `de5df0e`

The sandbox's persisted file snapshot was stale again: 55 paths differed from `origin/main`
(44 modified, 11 missing). Before touching anything, every differing file was inspected. All but
one of the modified blobs exist verbatim in committed history — the snapshot had reverted files
to older committed states; the missing paths are the ones snapshots drop (`.github/`,
`tools/vendor/host-debs/*.deb`, `tests/unit/test_ir_facts.cpp`). The one blob not in history
(`src/core/sigsys_diagnostics.cpp`, an older formatting/semantics variant) was preserved anyway:
the full pre-resync diff and file copies are in `/home/user/pre-p15-resync-backup/` (kept outside
the repo). Only then was the tree resynchronised: `git status` clean, `HEAD == origin/main ==
de5df0ea65f3c8166cc7827ecc9d84a350d4431a`. P14 stage 1 was confirmed in the actual checkout
(`IrFacts` at `include/hyperion/translator.hpp:84`, `compute_ir_facts` at
`src/core/translator.cpp:2332`, wired into pass 1 at `:2443`, 17 cases in
`tests/unit/test_ir_facts.cpp`). Baseline before any change: native (clang 19, RelWithDebInfo)
**370 passed / 0 failed / 47 skipped of 417**, `ctest` **19/19**.

#### Phase 1: what the dynamic block stream actually looks like

**Method.** Tier 1 chains blocks inside generated code, so the C++ dispatch loop never sees most
transitions; counting there would measure the chaining, not the guest. The probe therefore sat in
tier 0's `run_program` (temporary, env-gated `HYPERION_TRACE_PROBE=<path>`, never committed): it
reconstructs exactly the block stream the translator's own rules produce — a block closes at
`hlt`/`syscall`/`jmp`/`call`/`ret`/`jcc` or at the 256-instruction cap — and records per-block
executions, guest-instruction and byte lengths, exit kind, and every block-to-block edge.
Caveats, stated: the probe measures the run it instruments (the extra environment variable costs
glibc ~4 600 extra startup instructions, identical across guests; guest stdout md5 was verified
unchanged and equal to native execution for all four guests), and tier-0 frequencies are the
guest's, independent of which tier executes them.

| guest | retired steps | block execs | distinct blocks | dyn. mean len | transitions /1000 insns |
| --- | --- | --- | --- | --- | --- |
| sieve | 16 995 559 | 4 159 894 | 1 282 | 4.09 | 244.8 |
| nqueens | 39 368 251 | 4 302 096 | 1 199 | 9.15 | 109.3 |
| quicksort_c | 21 677 694 | 3 911 314 | 1 293 | 5.54 | 180.4 |
| qsort_glibc | 21 611 597 | 3 969 203 | 1 289 | 5.44 | 183.7 |

Dynamic exit kinds (share of block executions):

| guest | jcc | call | ret | jmp | syscall/hlt |
| --- | --- | --- | --- | --- | --- |
| sieve | 99.7 % | 0.1 % | 0.1 % | 0.1 % | ~0 % |
| nqueens | 59.9 % | **20.0 %** | **20.0 %** | 0.1 % | ~0 % |
| quicksort_c | 94.9 % | 0.3 % | 0.3 % | 4.4 % | ~0 % |
| qsort_glibc | 55.0 % | **20.0 %** | **20.0 %** | 5.1 % | ~0 % |

Branch direction at `jcc` (taken vs fall-through): sieve **97.6 % taken** (a loop back-edge),
nqueens 24.6 % taken / 75.4 % fall-through, quicksort_c 59.5 % / 40.5 %, qsort_glibc
49.6 % / 50.4 %. Only ~29–38 % of two-successor `jcc` blocks have a ≥90 % dominant direction
(median dominant share 78–83 %). `ret` targets are polymorphic (up to 35–36 distinct successors
per `ret` block; in nqueens essentially all 858 k `ret` executions are in multi-successor blocks);
indirect `jmp` is negligible (≤3 executions in multi-successor blocks). Concentration is extreme:
the top 5 blocks cover 99.1 % (sieve), 99.2 % (nqueens), 46.9 % (quicksort_c) and 72.4 %
(qsort_glibc) of all block executions.

**Realistic traces.** A NET-style greedy formation over the aggregate edge counts (follow the
dominant successor; stop at `call`/`ret`/`syscall`/`hlt`, at an already-claimed block, or at caps
of 16 blocks / 64 guest instructions; heads ≥0.1 % of executions) produces:

| guest | traces | mean length | mean guest insns | interior transitions removed | side-exit events |
| --- | --- | --- | --- | --- | --- |
| sieve | 1 | 2.0 blocks | 6 | 22.1 % of transitions | 1.9 % |
| nqueens | 1 | 3.0 blocks | 34 | 32.2 % | 7.3 % |
| quicksort_c | 10 | 2.3 blocks | 10.5 | 26.7 % | 7.9 % |
| qsort_glibc | 9 | 2.7 blocks | 13.4 | 27.0 % | 11.8 % |

Greedy hottest-path walks from the hottest starts confirm why traces stay short: they hit a
`call` or `ret` within 1–3 blocks (nqueens' hot cycle `…→ call →…→ ret →…` repeats every
iteration), or close a loop. Full-path probability collapses with length — quicksort_c's 5–7-block
candidate paths traverse end-to-end only 14–15 % of entries. And the single hottest edge in the
whole corpus, sieve's `0x401610 → 0x401610` (51.0 % of all its block executions), is a self-loop
the translator **already** compiles as a self-loop superblock (`translator.cpp:3412`).

**What a removed transition is worth today.** Transitions are not dispatches to C++: both `jcc`
successors and direct `call` exits are patched to direct branches inside the write window
(§29.7, §29.13; measured this session under tier 1: 496/496, 422/421, 542/543, 560/569 exits
linked for the four guests, dispatch misses only 7.4–8.5 k per run against millions of hits);
unlinked exits take the in-JIT jump-table probe, not a C++ return. What an interior trace edge
would actually save is the boundary state traffic: §29.15 measured *all* block-boundary traffic
(prologue loads, exit/helper spills, reloads) at **6.1–6.6 % of generated words**. Removing
22–32 % of transitions therefore has a static ceiling of ≈ **1.3–2.1 % of generated code** —
before subtracting side-exit synchronisation (1.9–11.8 % of transitions would pay a full
block-exit-style sync mid-trace), side-exit stub code, trace-entry duplication, and the fact that
a well-predicted direct branch is cheaper than the average generated word. The guest-state
operand traffic that dominates emission (~0.7 accesses per instruction, §29.24) is *inside*
blocks and is untouched by trace formation unless it is combined with the canonical cross-block
register assignment §29.15 already designed and deferred. Expected runtime effect: **≤1–2 %**,
at or below this rig's resolution (interleaved A/B CI half-widths 1–4 %, §29.20/§29.25), measured
through qemu rather than on AArch64.

**Interactions, checked against the implementation** (measured facts above; the rest is code
reading, labelled as such):

* *Direct linking (P7/P10):* complementary but overlapping — linking already removed the
  dispatcher from exactly the edges a trace would internalise; the trace's marginal saving is
  the register spill/reload pair, not the transition.
* *Cold-block gating (P6):* trace heads need a much higher bar than the compile threshold
  (`hot_` saturates at 3 reaches); the existing 1 024-entry direct-mapped hot table has no edge
  information at all, so any profile-driven variant needs new profile state.
* *Signals, faults, exceptions:* no new hazard in principle — a trace is one translation unit,
  so per-instruction fault stubs and the exit epilogue's state sync generalise — but every
  interior `jcc` needs a side exit that performs the *full* sync (pinned registers stored,
  `rip` = the exact next guest pc, PF/AF descriptor valid: §29.11's invariant), and any cycle
  inside a trace needs a budget check or a long trace loop could overrun `max_steps`, which
  `run()` currently guarantees per block.
* *Self-modifying code:* invalidation is already total — a code-epoch bump drops the entire
  cache (`TranslationCache::run`) — so traces need no new invalidation machinery, but duplicated
  hot blocks multiply the retranslation a bump costs.
* *W^X:* compatible only under the existing discipline — emit the whole trace and patch its
  internal links inside one write window, publish once (`translate_at`); no published page is
  ever re-opened. Feasible, but trace/link patching must not outlive the window.
* *Cache growth / i-cache:* selective traces duplicate few blocks (≤24 blocks/guest above,
  ≈ ≤8 KB), so growth is not the blocker; it becomes one only if formation is made aggressive,
  which the side-exit rates argue against.

#### Phase 2: designs compared, and the smallest one that would be built

1. **Hot-edge trace formation (NET-style, profile-recorded traces).** Needs edge profiling in
   generated code (new hot-path counters), a trace recorder, trace cache keyed by head, and
   anchor patching. Most machinery, targets exactly the measured-hot paths — but the measured
   traces are 2–3 blocks, so the machinery buys the least where it costs the most. Rejected
   as first step.
2. **Superblocks with side exits (no profiling).** At translation time, continue the unit past
   a `jcc` along one successor and give the other a side exit to the existing dispatcher.
   Which successor is "the path" must be guessed statically (fall-through is right 75 % in
   nqueens, wrong 98 % in sieve), and the unit needs mid-unit conditional exits with full state
   sync — a new emission shape, and a new way to break the §29.3 write-back invariant.
3. **Selective formation on proven-hot paths only (recommended, if this is ever built).**
   Reuse the block machinery: a *trace* is a `CachedBlock` whose translation unit is the head
   block extended through dominant successors, formed only for heads that P6's gating has
   already promoted and that additionally exceed a high execution bar. Design, specified so a
   future implementation does not have to re-derive it:
   * *Selection:* head executed ≥1 000 times (profile counted at the C++ funnel is too coarse —
     chained executions bypass it — so this requires either interpreter-collected profiles or
     in-JIT counters; that dependency is itself a cost); extend via the successor taken
     ≥80 % of the time; stop at `call`/`ret`/`syscall`/`hlt`, indirect branches, a block
     already in the trace, 16 blocks, or 64 guest instructions.
   * *Exits:* every non-dominant successor and every trace-ending transfer is a side exit to
     the *original* block's entry or the dispatcher, after full state sync (all pinned
     registers stored, guest `rip` set, PF/AF descriptor materialised); the original blocks
     remain translated and reachable, so a side exit is never a correctness event.
   * *Budget:* a budget check on every back-edge inside the trace; retired-instruction
     accounting stays one `Instruction` = one step (§29.18's invariants).
   * *Invalidation:* unchanged — code-epoch bump invalidates everything; traces are dropped
     with it and re-formed only when the head re-earns its threshold.
   * *W^X:* whole trace emitted, internally linked and published in one write window; no
     patching after publish; trace entries live in the same arena.
   * *Fallback:* any doubt (decode failure mid-trace, successor not yet translated, delta out
     of branch range) → emit the plain head block; the un-traced path is always available and
     is the default until a head qualifies.

#### Phase 3: success criteria, fixed before any implementation

*Correctness:* full native + AArch64-under-qemu suites green with no test weakened; guest
stdout and `--timing` step counts identical to the un-traced build on all four guests; a
differential run (tier 0 vs tier 1) over the ISA corpus guests; SMC, fault-pc and step-limit
behaviour exercised by the existing tests that pin them. *Performance:* interleaved A/B on the
§29.20 rig (`--timing`, ≥12 pairs × 2 sessions per guest, bootstrap 95 % CI on the paired
median ratio), accepted only if the median ratio is **≤0.98 with the CI entirely below 1.0 on
at least two guests and no guest regressing beyond 1.01**; translation time, `total_code_bytes`
(growth ≤10 %), and block/trace counts reported alongside. *Rejection conditions:* CI straddles
1.0, any correctness delta, code growth >10 %, or side-exit rates above the Phase-1 model —
any one of these retires the prototype, as with §29.21 and §29.24.

#### Phase 4: decision

Not implemented. The measured ceiling (≈1.3–2.1 % of generated code, ≤1–2 % runtime, through an
emulator-mediated rig whose CI half-widths are 1–4 %) does not clear the bar Phase 3 sets, the
hottest paths are already captured by the self-loop superblock and direct linking, the
call/ret-dominated guests break traces every iteration, and the only version worth building
depends on edge profiling the translator does not have. This is the fourth measurement in a row
(§29.15, §29.22, §29.24, here) whose conclusion is that block granularity is not where the
remaining cost lives; the state-traffic problem (~0.7 guest-state accesses per instruction) is
an allocator problem, and traces without the canonical assignment of §29.15 do not touch it.

**Reversal conditions.** Revisit when any of these holds: (a) a native-AArch64 measurement rig
with ≤1 % noise exists, making a 1–2 % effect decidable; (b) the canonical cross-block register
assignment (§29.15) is undertaken for its own sake — a trace is its natural unit, and the two
together address both the 6 % boundary share and the in-block state traffic; (c) a workload
with long, hot, acyclic paths (the four benchmark guests have none: their hot cycles are
≤3 blocks or cross a call) becomes a stated target.

### 29.27 P16 — cross-block register allocation: the opportunity is real, the prototype works, and this rig cannot certify it — deferred

§29.15 deferred cross-block register allocation after measuring only the *boundary* traffic
(6.1–6.6 % of generated words). P15 (§29.26) closed by noting the remaining cost is in-block
guest-state traffic and that a trace would be the natural unit for a canonical assignment. P16
reopened the question with a sharper instrument and a pre-registered decision rule. Outcome:
**defer** — a working prototype shows a real but small effect that the available hardware cannot
separate from its own bias, at +9 % code size. The prototype was removed; production code is
unchanged.

#### Baseline and integrity

The stale-snapshot state was inspected before any resync (44 modified / 11 deleted / 0
untracked; every modified blob except the known `sigsys_diagnostics.cpp` variant exists in
committed history; full backup in `/home/user/pre-p16-resync-backup/`). Tree synchronised to
`HEAD == origin/main == ef24432d75f6c8e8b8c58470d5bd03ab34481776`, P14 stage 1 confirmed
(`compute_ir_facts`, `translator.cpp:2332`). Baseline, recorded before any change: native
(clang 19.1.7, RelWithDebInfo, tests+benchmarks) **370 passed / 0 failed / 47 skipped of 417**,
`ctest` **19/19**; AArch64 cross (aarch64-linux-gnu-g++ 14, static) under
`qemu-aarch64-static`: **417 passed / 0 failed / 5 skipped of 422**; `cli_smoke` green on both.
The same numbers were re-verified on the final tree after the prototype was removed.

#### What "0.7 state accesses per instruction" actually is — definition first

A temporary census in `translate_cached_block` (uncommitted, removed) classified every emitted
main-body load/store with base `x8`/`x27` by `GuestState` offset (unsigned-immediate forms;
out-of-line slow/fault stubs excluded), and a tier-0 probe supplied dynamic block executions.
The often-quoted ~0.7 figure is only the **guest-GPR load** column. The full picture
(translated blocks, static / dynamically weighted):

| guest | static total | dyn. total | dyn. GPR | dyn. flags | dyn. PF/AF | dyn. rip |
| --- | --- | --- | --- | --- | --- | --- |
| sieve | 2.886 | 3.329 | 1.320 | 1.037 | 0.733 | 0.239 |
| nqueens | 2.905 | 2.242 | 1.303 | 0.437 | 0.327 | 0.175 |
| quicksort_c | 2.866 | 2.774 | 1.299 | 0.698 | 0.523 | 0.254 |
| qsort_glibc | 2.802 | 2.941 | 1.347 | 0.913 | 0.410 | 0.271 |

So GPR traffic — the only part a register allocator can touch — is ~1.30 accesses per guest
instruction dynamically; flags+PF/AF are of the same order (and belong to P9's problem, not this
one). State accesses are 26–33 % of dynamically executed main-body words. Architecture, for the
record: `GuestState` is authoritative at every block entry/exit; cached blocks pin ≤5 guest
registers in `x19..x23` chosen per block (`refs > 2`), `x24..x28` hold dispatch/budgets/state/
context; synchronisation happens at helper calls (spill+reload), halt/syscall (spill), fault
stubs (spill), and every dispatch-bound exit (spill). A facts-based access model
(unpinned touch = 1 access; pinned = load+store) reproduces the census for the current policy
within 0–15 % per guest.

#### Opportunity, modelled before any code

Values actually live across edges are few (0.78–2.43 predecessor-produced values per transition;
successors read 2.64–4.98 values at entry), but a *canonical* assignment — the same five guest
registers in `x19..x23` in every block, never synced on JIT-internal transitions — also removes
the in-block touches of those registers in blocks that pin nothing today. Modelled upper bounds
(canonical sync charged only at C++ funnels), by guest-chosen set: −51 to −99 % of GPR accesses.
A single global set must include `rsp` (`{rax, rdx, rsp, rsi, rcx}`): without it nqueens
*regresses* +3.0 % of executed words (its hot code touches `rsp` 31 % and `r14`/`r15`); with it
all four guests improve on paper by **−4.1 to −8.5 % of executed words**. A canonical-3
(`{rax, rdx, rsp}`) plus two per-block flex pins models slightly worse (−3.5 to −7.2 %) with
less code growth and more boundary complexity. Designs A (within-block caching) is the status
quo, already threshold-swept flat in §29.22; C (trace-local) couples this to the rejected P15;
B (pure canonical) and D (hybrid) are the candidates, B being the smallest change that tests
the hypothesis.

#### Pre-registered acceptance criteria (fixed before candidate measurement)

Interleaved A/B on the §29.20 rig (`--timing`, `env -i`, ≥12 pairs × 2 sessions per guest,
bootstrap 95 % CI on the paired median ratio, candidate/baseline): production implementation
requires median ratio ≤ 0.98 on at least two guests with CIs supporting a genuine improvement,
no guest regressing >1 % (median), no correctness regressions or new skips, code-size growth
≤10 %, translation overhead acceptable. If the rig cannot resolve ~2 %, the result is reported
as inconclusive, not successful.

#### Prototype (temporary, uncommitted, removed after measurement)

The smallest change that carries values across boundaries: pass 1.5 always maps
`{rax, rcx, rdx, rsp, rsi}` → `x19..x23`; per-block entry reloads deleted; spills deleted at
dispatch-bound exits only (`jmp`/`call`/`ret`/`jcc`/fall-through/self-loop); spills *kept* at
helper calls, halt, syscall, and fault stubs; the entry trampoline loads the canonical set
once; the dispatcher-miss path and the budget-exhaustion stub (`entry_budget_undo`, which
reaches the C++ epilogue without passing a spill today) gained the 5 stores that keep
`GuestState` authoritative at every C++-visible point. The dispatcher trampoline itself never
touches `x19..x23` (verified by reading `build_jit_trampolines`).

Correctness: AArch64 suite **416 passed / 1 failed / 5 skipped** — the single failure is
`translator.translated_code_size_is_bounded`, the deliberate small-block code-size guard
(canonical charges 5-word syncs in every block's cold stubs); no correctness test failed.
Guest stdout identical to baseline and to native for all four guests. Code size grew
**+8.9 % / +8.2 % / +9.2 % / +8.6 %** (sieve/nqueens/quicksort_c/qsort_glibc); translation time
unchanged (sieve medians 45.4 ms vs 46.0 ms, interleaved).

#### Candidate measurements (qemu-mediated, paired `--timing` run wall)

| guest | session 1 | session 2 | pooled (24 pairs) | A/A control (identical binaries) |
| --- | --- | --- | --- | --- |
| sieve | 0.957 [0.913, 0.984] | 0.970 [0.954, 0.999] | **0.963 [0.945, 0.988]** | 0.994 [0.975, 1.027] |
| nqueens | 0.970 [0.959, 0.989] | 0.986 [0.950, 1.011] | **0.978 [0.963, 0.995]** | 0.984 [0.967, 1.017] |
| quicksort_c | 0.992 [0.972, 1.025] | 0.997 [0.971, 1.020] | 0.994 [0.974, 1.011] | 1.014 [0.996, 1.079] |
| qsort_glibc | 0.994 [0.970, 1.018] | 0.999 [0.967, 1.037] | 0.999 [0.980, 1.016] | 1.010 [0.999, 1.024] |

#### Decision: defer (prototype only, removed)

The direction is consistent (7 of 8 guest-sessions below 1.0; pooled sieve and nqueens CIs
exclude 1.0), so this is not a rejection — the effect is very likely real and worth ~2–4 % on
sieve, ~2 % on nqueens, ~0 % on the quicksorts. But the A/A controls show this rig manufactures
apparent effects of up to 1.6 % (nqueens A/A median 0.984) with *identical* binaries: the
claimed nqueens effect is within 2× of the rig's demonstrated bias, session 2 does not
reproduce session 1's CI, and the price is +9 % code size, a broken code-size guard, and a
permanent relaxation of the §29.3 write-back discipline at every JIT-internal boundary — the
invariant two earlier items called non-negotiable. Under the pre-registered criteria that is
"inconclusive on this hardware", and an inconclusive 2 % does not buy that risk.

**Reversal conditions.** Revisit when (a) a native-AArch64 rig with ≤1 % noise exists — the
pooled CIs say the effect would very likely certify there; (b) someone first reduces the
static cost (canonical-3 + flex pins, or canonical sync only where stubs exist), bringing
code growth well under the current +9 %; or (c) a workload appears whose hot code is
GPR-touch-dominated like sieve's but larger, where the modelled −8 % of executed words has
room to show. Until then the per-block allocator (§29.22: threshold swept, flat) stays.

### 29.9 Still open from §L

Items 1-5 of §L are all resolved (1 justified and pinned, 2 and 5 implemented, 3 retracted as wrong,
4 implemented), and item 7 (direct linking) is implemented as well — §29.7 — because P5's write
window made it cheap. §26 rows 1, 2, 3, 4, 5 should be read with §29 in mind; the rest of the table
stands.

Item 11 (inline the remaining helper opcodes) is done — §29.8 — with the work chosen by counting
rather than by the audit's opcode estimate. Item 9 (lazy flags) was profiled in §29.10, which
concluded that a full six-flag descriptor is architectural; **stage 1 of that design (PF/AF only) is
implemented and measured in §29.11**. Item 6 (hot-block gating) is done and measured in §29.12:
below a crossover of tens of thousands of guest instructions, compiling on first reach was paying
more than it saved, and the cache now interprets cold block starts instead. Item 10 (shadow stack) was
measured in §29.13: the `call` side was worth implementing (direct calls are now linked by the P7
machinery, 9 % on a call-heavy guest) and the shadow-stack half was rejected in favour of a
return-target cache that belongs with item 14. Item 9 stage 2 (CF/ZF/SF/OF via
`cc_op`/`cc_src`/`cc_dst`) was measured in §29.14: flag materialisation is 10.2–10.8 % of generated
code, the deferrable part of it is ≈4 % of executed code, and a paired A/B of the *existing* PF/AF
deferral cannot resolve 5 words per producer in this environment (ratio median 1.02, spread
0.89–1.21) — so it is deferred, not attempted, with the required design recorded. Item 8 (cross-block register allocation) was
measured in §29.15: the allocator pins 0.65–0.75 registers per block, so all boundary traffic
together is 6.1–6.6 % of generated code and only part of that is removable — below the
measurement resolution of §29.14, so it is designed but deferred. Item 12 (signals) is audited and partly
implemented in §29.16: `kill`/`tgkill` now honour the guest's disposition and mask instead of
ending every run, with handler invocation (the x86-64 frame plus `rt_sigreturn`) and asynchronous
delivery documented as absent. Item 13 (guest threads) is audited in §29.17: the supported model is
"many processes, no threads", every shared-state clone flag is refused (and the refusal is now
pinned at unit level as well as end to end), and the six things a thread implementation would need
- per-thread state and JIT context, a write-race-free dispatcher, a translation-cache shootdown,
futex wait queues and asynchronous signals - are written down instead of approximated. Item 14 (an IR) is proposed and staged in §29.18, and then executed: stage 1
(the facts pass) shipped in §29.23 with byte-identical output, and stage 2 (block-level copy
propagation) was measured and **rejected** in §29.24 — the corpus contains at most 2 removable
copies per guest, 0.04–0.08 % of translated instructions, and renaming saves zero words. Nothing
in §L items 6–14 is left unaddressed; §30 is the summary.

---

---

## 30. Status after items 6–14

| item | outcome | where |
| --- | --- | --- |
| 6 hot-block gating | **implemented** — cold block starts execute in the interpreter; 14–35 % faster from 10 k guest instructions up, 69 % less generated code | §29.12 |
| 8 cross-block register allocation | **measured, deferred** — 0.65–0.75 registers pinned per block, so all boundary traffic is 6.1–6.6 % of code and only part of it is removable | §29.15 |
| 9 stage 2 (CF/ZF/SF/OF) | **measured, deferred** — flag materialisation is 10.2–10.8 % of code, the deferrable part ≈ 4 %, under this environment's noise floor | §29.14 |
| 10 shadow stack | **half implemented** — direct `call` exits are linked (9 % on a call-heavy guest); the shadow stack itself is rejected, a verified ret-target cache deferred to P14 | §29.13 |
| 12 signals | **audited and partly implemented** — `kill`/`tgkill` honour disposition and mask; handler invocation, `rt_sigreturn` and asynchronous delivery documented as absent | §29.16 |
| 13 threads | **audited and pinned** — "many processes, no threads"; every shared-state clone flag refused, the six pieces a thread implementation needs written down | §29.17 |
| 14 IR | **stage 1 shipped, stage 2 rejected on measurement** — facts pass with byte-identical output (2 047 blocks, 4 guests); block-local copy propagation can remove 0.04–0.08 % of instructions and saves 0 emitted words | §29.23, §29.24 |
| 15 trace/superblock formation | **investigated, designed, not implemented** — dynamic traces are 2–3 blocks; removing 22–32 % of transitions has a ≈1.3–2.1 % generated-code ceiling (≤1–2 % runtime), under the rig's noise floor; hottest edge already a self-loop superblock, call/ret break traces in two guests | §29.26 |
| 16 cross-block register allocation | **prototyped, measured, deferred** — canonical `{rax,rdx,rsp,rsi,rcx}` in `x19..x23`: guest outputs identical, pooled A/B 0.963 (sieve) / 0.978 (nqueens) with CIs below 1, but A/A controls show ≤1.6 % rig bias, quicksorts flat, code size +8.2–9.2 %; needs a native-AArch64 rig to certify | §29.27 |

Verification for the batch, at `24f6477` (P14 stage 1): native (x86-64, clang-19) 370 passed / 0
failed / 47 skipped of 417, `ctest` 19/19; AArch64 under `qemu-aarch64-static` 417 passed / 0
failed / 5 skipped of 422; at `2303e54` the same batch was 353/0/47 of 400 and 400/0/5 of 405; GCC and clang strict builds (`HYPERION_WARNINGS_AS_ERRORS=ON`) clean; full guest
runs on both tiers byte-identical (`acc=9000001 counter=0`, `acc=20475694797959 g=50991808`).

Two measurements in this batch are negative results that matter for planning: the PF/AF deferral
of §29.11 did not separate from noise on whole-process timing (paired-ratio median 1.02, spread
0.89–1.21), and flag materialisation as a whole is ~10 % of generated code. §29.20 then built the
rig that removes that excuse (`--timing`, 1–4 % CI half-widths), which is why the remaining items
are now described as *sequenced* rather than *unmeasurable*.

---

## Appendix — evidence index

| Claim | Evidence |
| --- | --- |
| Tier 1 unreachable on x86-64 | `exec_mem.cpp` `executable_supported()` `#else return false`; `main.cpp:164,250`; `hyperion_tier_bench` output "tier 1: unavailable" |
| Tier 1 executes under qemu | `build-aarch64` + `qemu-aarch64-static`: unit 360/0/5, `tier 1: 1210 basic block(s) translated` |
| Block-local regalloc, 5 registers | `translator.cpp:1467-1469`, `:2163-2192` |
| Flags as 6 bytes, eager | `guest_state.hpp` `struct Flags`; `translator.cpp:283-328` |
| In-JIT dispatch, hash probe | `translator.cpp:2054-2073` |
| Self-loop superblock | `translator.cpp:3033-3038`, `:3152-3168` |
| 2 mprotect per block | `exec_mem.cpp:170-190` |
| 16 KiB flush per syscall | `interpreter.hpp:38-55` (512×16 B ×2); `translation_cache.cpp:241`; `interpreter.cpp:2320`; measured 744 ns vs 131 ns dispatch — **now conditional, see §29.2** |
| LOCK ignored | `decoder.cpp:656-666` sets `insn.lock`; no reader anywhere — **benign, see §29.1** |
| Threads refused | `process.cpp:371-375`, `:604-606` |
| Signals never delivered | **partly superseded by §29.16** (P12): `kill`/`tgkill` now honour disposition and mask, but no frame is ever built and there is still no `rt_sigreturn` |
| fork = host fork + deep copy + full retranslate | `process.cpp:407,437,508,556` |
| No hot-block detection | **superseded by §29.12** (P6): a per-PC count gates cold block starts to the interpreter; the only thing still absent is an execution-frequency profile feeding *other* decisions |
| Tier 1 aborts on untranslatable block | `translation_cache.cpp:184-190` |
| No exception boundary | `grep try/catch src/cli/main.cpp src/core/translation_cache.cpp` → none |
| AArch64-gated tests skip on x86-64 | `hyperion_unit_tests` SKIP list — 47 skip natively / 5 under qemu at `2303e54`, each with its reason printed |
