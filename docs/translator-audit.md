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

### 29.9 Still open from §L

Items 1-5 of §L are all resolved (1 justified and pinned, 2 and 5 implemented, 3 retracted as wrong,
4 implemented), and item 7 (direct linking) is implemented as well — §29.7 — because P5's write
window made it cheap. §26 rows 1, 2, 3, 4, 5 should be read with §29 in mind; the rest of the table
stands.

Item 11 (inline the remaining helper opcodes) is done — §29.8 — with the work chosen by counting
rather than by the audit's opcode estimate. Item 9 (lazy flags) was profiled in §29.10, which
concluded that a full six-flag descriptor is architectural; **stage 1 of that design (PF/AF only) is
implemented and measured in §29.11**. Still open: item 6 (hot-block gating), 8 (cross-block register
allocation), 9 stage 2 (CF/ZF/SF/OF via `cc_op`/`cc_src`/`cc_dst`), 10 (shadow stack), 12 (signal
delivery), 13 (guest threads), 14 (an IR and a real optimiser).

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
| Signals never delivered | `syscall.cpp:2042-2095` (record only); comment at `:2045-2050`; no `rt_sigreturn` |
| fork = host fork + deep copy + full retranslate | `process.cpp:407,437,508,556` |
| No hot-block detection | `translation_cache.cpp` entire file; counters unused |
| Tier 1 aborts on untranslatable block | `translation_cache.cpp:184-190` |
| No exception boundary | `grep try/catch src/cli/main.cpp src/core/translation_cache.cpp` → none |
| 24 AArch64-gated tests skip on x86-64 | `hyperion_unit_tests` SKIP list (still 24 after the post-audit change; 5 skip under qemu) |
