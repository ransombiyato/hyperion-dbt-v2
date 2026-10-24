# ROADMAP

Staged plan for Project HYPERION. Each phase has **exit criteria that must be
demonstrated by evidence**, not merely by the existence of code.

Status legend: ✅ complete · 🟡 in progress · ⬜ not started

**Overall: Phases 0–7 complete for single-threaded static x86-64 Linux ELF64
binaries (104.5× microbench speedup; 5.17×–20.12× end-to-end speedup across 5
real `gcc 14.2 -O2 -static` `glibc 2.41` C workloads totaling `120,313,691`
dynamic instructions).**

| Phase | Title | Status |
| --- | --- | --- |
| 0 | Project foundation | ✅ complete |
| 1 | Establish the baseline | ✅ complete (UML item blocked, see note) |
| 2 | Minimal instruction execution | ✅ complete |
| 3 | First native code generation | ✅ complete |
| 4 | Basic dynamic binary translator | ✅ complete |
| 5 | Performance measurement and optimization | ✅ complete |
| 6 | Linux ABI and syscall compatibility | 🟡 static + dynamic ELF64, process creation, **128 of 375** syscalls answered (122 implemented, 6 stubbed), dispatch engine + external JSON registry complete (threads, sockets, signals remaining) |
| 7 | Advanced CPU compatibility (baseline psABI + 128-bit SSE2) | ✅ baseline x86-64 + SSE2 complete |
| 8 | AI optimization laboratory | ⬜ not started |
| 9 | Real application compatibility | 🟡 5/5 static `glibc 2.41` C workloads verified |
| 10 | Stabilization and release | ⬜ not started |

---

## Scope correction (2026-10-19)

The original Phase 0 plan stated that kernel/system-mode emulation was an
explicit non-goal. **That assumption has been withdrawn.** It was made before
any measurement of the reference device and it capped the achievable
compatibility below the project's actual goal, which is a broadly compatible
Linux execution environment — not merely an instruction translator.

What changed, and what did not:

- **Unchanged:** the user-mode translator is the primary path, and Phases 2-5
  build it. Nothing in the phase order moves.
- **Changed:** kernel-level compatibility is now in scope for *investigation*
  (`DECISIONS.md` D-011, D-012). A measured experiment showed that a real Linux
  kernel can run as an unprivileged process, with `systemd` reaching
  `multi-user.target`; an AArch64 port of that mechanism exists.
- **New constraint:** the Phase 6 syscall layer must sit behind an interface so
  the same translator can direct syscalls either to the host kernel or to a
  user-space kernel. This is a design constraint, not a feature to build now.
- **New risk:** the AArch64 port is unmerged and untested on the reference
  device (R7, R1 in `docs/architecture-feasibility.md`).

Full analysis and evidence: [`docs/architecture-feasibility.md`](docs/architecture-feasibility.md).
No kernel-level code is to be written until the tier is validated on the
reference device.

---

## Phase 0 — Project foundation ✅

**Objective.** A buildable, testable, documented repository that a new agent
can understand without the original conversation.

**Delivered.**

- CMake/C++20 project building clean with `-Werror` under GCC and Clang.
- Core library (`version`, `host_probe`) with public headers.
- Dependency-free test harness, wired into CTest.
- Cross-architecture validation pipeline: AArch64 cross-build executed under
  `qemu-user`, proven working in CI-equivalent local runs.
- Host benchmark harness emitting machine-readable JSON with guest-execution
  metrics explicitly marked unavailable.
- Full documentation set, CI workflow, reproducible bootstrap script.
- Local git history with a coherent milestone commit.

**Exit criteria — evidence.**

| Criterion | Evidence |
| --- | --- |
| Project builds | `scripts/dev.sh build` succeeds; see STATUS.md |
| Tests pass | `scripts/dev.sh test` → 2/2 tests passed |
| Cross-arch pipeline works | `scripts/dev.sh cross-test` → 2/2 passed, AArch64 binary ran under qemu |
| Documentation committed | All files listed in README index present |
| New agent can resume | `docs/agent-handoff.md` describes state and next task |

**Explicitly not done.** No decoder, no IR, no interpreter, no code generator,
no syscall layer.

---

## Phase 1 — Establish the baseline ✅ (item 6 blocked, see note)

**Objective.** Know the prior art and have a reproducible measurement harness
*before* writing an engine.

**Work items.**

1. Investigate FEX-Emu, Box64, QEMU TCG and relevant decoder/compiler projects.
2. Document for each: reuse potential, licence, design trade-offs, comparison
   against a custom implementation.
3. Decide the reuse-vs-build boundary and record it in `DECISIONS.md`.
4. Build a reproducible benchmark harness: record host CPU, OS, compiler,
   build configuration and methodology.
5. Record baseline numbers where obtainable; mark unavailable measurements as
   unavailable — **never fabricate**.
6. **(added by the scope correction, optional)** Cross-build the AArch64 UML
   port and confirm the binary is produced.

**Exit criteria.**

- `THIRD_PARTY.md` and a baseline comparison document are committed. ✅
- `benchmarks/` produces repeatable, attributable results. ✅
- `BENCHMARKS.md` documents methodology and any measured baselines. ✅

**Evidence.** `docs/prior-art.md` (survey, licences, trade-offs, reuse matrix);
`DECISIONS.md` D-013 (reuse-vs-build boundary); `BENCHMARKS.md` §4 (measured
host calibration with spread and a scaling validation); `install.sh` +
`scripts/hyperion` (one-line install and a `hyperion` test-run command).

**Item 6 note.** The AArch64 UML port configures but does not compile. The
failure is not arch-specific — stock x86-64 UML fails identically on this host —
so it is a host glibc/kernel-header incompatibility, not a port defect. The port
is recorded as *plausible but unverified*; no claim is made that it works.
Transcript in `docs/prior-art.md` §6.

**Defect found and fixed in this phase:** BENCH-001, a benchmark harness that
reported a fabricated result because the optimiser eliminated the timed
workload. See `docs/known-issues.md`.

---

## Phase 2 — Minimal instruction execution 🟡

**Objective.** Correctly execute a small, explicitly documented instruction
subset.

**Work items.**

1. Define the subset in `COMPATIBILITY.md`. ✅ **Done** — §2 is now a precise
   specification of the implemented subset (and lists what is *not* implemented
   separately, so the plan is not mistaken for reality).
2. Implement the decoder for that subset. ✅ **Done** —
   `src/core/decoder.cpp`; unsupported input is rejected with a specific
   `DecodeError`.
3. Implement guest state and instruction semantics in the IR/interpreter.
   ✅ **Done** — `guest_state.*`, `interpreter.*`.
4. Build automated tests for registers, flags, arithmetic, memory, control flow.
   ✅ **Done** — 54 unit cases.
5. Differential-test the interpreter against an independent reference.
   ✅ **Done** — 8 cases against **real x86-64 hardware** (`tests/differential/`).

**Exit criteria.** The supported subset passes differential tests against an
independent reference. Unsupported instructions fail explicitly, never silently.
**Met for the implemented subset.** Not met for the *planned* items in
`COMPATIBILITY.md` §2.5 (`test`, `neg`, `not`, shifts, `imul`, `lea`, real
`call`/`ret`, `jcc rel32`, `syscall`, `div`), which are deferred.

**Remaining before Phase 3:** a convenience helper to run a multi-instruction
program image (with branches/loops) through the interpreter for testing. The
stepping semantics already exist.

---

## Phase 3 — First native code generation 🟡

**Objective.** Execute the Phase 2 subset through generated AArch64 code.

**Work items.**
1. AArch64 encoder/emitter — **Done.** `a64::Encoder`
   (`include/hyperion/aarch64_encoder.hpp`), every encoding reference-verified
   against GNU as/objdump (12 unit cases).
2. Executable memory management — **Done.** `ExecMem`
   (`include/hyperion/exec_mem.hpp`), W^X write-then-execute plus I-cache
   clear.
3. Executing generated code end to end — **Done for the encoder's subset.**
   `test_exec_mem.cpp` generates and *runs* `add` and a summation loop; both
   pass on AArch64 under `qemu-user` (and in CI's cross job), and skip
   honestly on x86-64.
4. Guest-state marshalling — **Done (increment 1).** A translated block has the
   signature `BlockResult block(GuestState*)` (AAPCS64, state pointer in `x8`);
   guest registers stay in the `GuestState` struct in memory and generated code
   loads/stores them. No guest-register allocation yet (Phase 5).
5. Translating decoded x86 instructions to AArch64 — **Done for the
   register/immediate subset.** `translate_instruction`
   (`include/hyperion/translator.hpp`) lowers `mov`, `add`, `sub`, `and`, `or`,
   `xor`, `cmp`, `inc`, `dec`, `nop` and `hlt` with 32/64-bit register or
   immediate operands. All six x86 flags (CF/PF/AF/ZF/SF/OF) are *computed in
   generated code* from AArch64 NZCV plus two ALU identities, not delegated to
   the interpreter. Control flow (`jmp`/`jcc`/`ret`) and the stack
   (`push`/`pop`) are refused explicitly (Phase 4); memory operands are refused
   explicitly (need an address space, Phase 6). A 847-case differential test
   executes the generated blocks under `qemu-user` and compares every register
   and all six flags against the interpreter.

**Exit criteria.** Verified test programs execute correctly through generated
AArch64 code, validated under `qemu-user` on x86-64 hosts and natively on
AArch64 hosts. **Met for the translated subset:** decoded x86-64 instructions
now execute through generated AArch64 code, and the differential suite is
exact against the reference interpreter. Multi-instruction *programs* (loops,
branches) remain Phase 4, so this is the completion of Phase 3's stated scope
rather than of program-level translation.

---

## Phase 4 — Basic dynamic binary translator 🟡 (core done, closing out)

**Objective.** A working translator loop with a code cache.

**Work items.** Translation blocks, translation cache, guest PC tracking,
block exits, cache invalidation, explicit handling of unsupported instructions.

**Status.** Core implemented and verified under `qemu-user`:

| Work item | State |
| --- | --- |
| Translation blocks | **Done** — `translate_block` decodes to a terminator |
| Guest PC tracking | **Done** — generated block writes the authoritative `rip` |
| Block exits | **Done** — `BlockExit` (fall-through / jmp / jcc / halted) |
| Translation cache | **Done** — keyed by guest PC; `TranslationCache` |
| Dispatcher loop | **Done** — `TranslationCache::run`, step-limited |
| Explicit unsupported handling | **Done** — stops with `kUnsupported`, never miscompiles |
| Cache invalidation | **Not started** — the cache never invalidates (documented boundary; Phase 7) |
| Block chaining | **Not started** — Phase 5 |

**Exit criteria.** A repeatable suite of small guest programs runs correctly.

**Evidence.** `translator.differential_program_loops` (counted loop) and
`translator.differential_branch_directions` (taken/not-taken) compare the final
`GuestState` and step count against `run_program` and pass under `qemu-user`;
`translator.dispatcher_*` prove unsupported instructions stop the run and an
infinite loop is reported as a step limit rather than hanging. The
`hyperion run --tier=1` CLI executes a whole program through generated AArch64
code (3 basic blocks translated) and agrees with tier 0.

---

## Phase 5 — Performance measurement and optimization 🟡

**Objective.** Measured, reversible optimization.

**Work items.** Baseline profiling; experiment independently with register
allocation, block chaining, faster decoding, lower-overhead dispatch,
instruction selection, constant propagation, dead-code elimination, code-cache
policies, multi-tier compilation. Preserve a known-good implementation; reject
changes failing correctness tests or regressing performance.

**Progress.**

- ✅ **Baseline recorded before any optimisation.** `hyperion_tier_bench` runs a
  fixed guest loop through both tiers; measured under `qemu-user`, Tier 1 is
  ~12.8× faster than the reference interpreter (38.4 vs 491.9 ns per guest
  instruction, IQR < 2%). Full record and caveats in `BENCHMARKS.md` §4.2. This
  is a *relative* signal between Hyperion's own tiers, not a hardware claim.
- ✅ **Dead-code elimination, part 1: parity fast path.** `emit_parity` XOR-folds
  with shifted `EOR` (19 → 6 emitted instructions). Generated code 904 → 644
  bytes (−28.8%, deterministic); Tier 1 −17.5% in an interleaved A/B.
  `BENCHMARKS.md` §4.2.1.
- ✅ **Dead-code elimination, part 2: per-instruction `rip` stores.** Blocks write
  `rip` once at exit instead of after every body instruction. Generated code
  644 → 584 bytes (−9.3%); Tier 1 ≈ −4%. `BENCHMARKS.md` §4.2.2.
- ✅ **Dead-code elimination, part 3: flag liveness.** `translate_block` decodes
  the block, runs backward flag liveness, then emits; each ALU instruction
  computes only the flags read before being overwritten. Generated code
  584 → 352 bytes (−39.7%, deterministic); size-guard program 148 → 90 words.
  `BENCHMARKS.md` §4.2.3. Guard: `translator.dead_flags_are_not_computed`.
- ✅ **Lower-overhead dispatch.** A 4096-entry direct-mapped table answers the
  next-PC lookup before the hash map (which stays authoritative). Measured
  99.9985% hit rate on the fixed workload. `BENCHMARKS.md` §4.2.4.
- ✅ **Instruction selection: ALU immediate folding.** Foldable `add/sub/cmp`
  immediates lower to one ADDS/SUBS-immediate when AF is dead. Size-guard program
  90 → 88 words. `BENCHMARKS.md` §4.2.5.
- ✅ **Register allocation (within a block).** The block's most-used guest
  registers are held in callee-saved host registers (x19..x28) and saved/restored
  around the block, with a *profitability guard* that declines allocation when
  the prologue/epilogue would cost more than the saved loads. On a new `--wide`
  workload (12-instruction body, 11 guest registers): 620 → 608 bytes and
  10.70 → 9.05 ns/guest-instruction. `BENCHMARKS.md` §4.2.6. Guards:
  `translator.differential_register_allocation_many_registers`,
  `translator.differential_register_allocation_immediates`. This work also found
  and fixed a pre-existing halted-`rip` bug (`BENCHMARKS.md` §4.2.7).
- ⬜ Block chaining.
- ⬜ Multi-tier compilation.

**Exit criteria.** Reproducible performance measurements and documented
optimization decisions.

---

## Phase 6 — Linux ABI and syscall compatibility 🟡 (128/375 syscalls, dispatch engine complete)

**Objective.** Run real Linux guest programs.

**Work items.** Guest syscall layer, ABI-sensitive structures, memory mapping,
file descriptors, signals, threading, futexes. Feature matrix + integration tests.

**Progress.**

- ✅ **Syscall inventory established (evidence-based).** The complete x86-64
  syscall set (375 `common`/`64` entries, Linux v6.12) is classified into the
  seven required files `docs/syscalls/U1.md`–`U4.md` and `E1.md`–`E3.md`,
  generated from `docs/syscalls/inventory.tsv`. Tiers are backed by a
  reproducible strace sweep (`scripts/trace-syscalls.sh`) rather than invented
  frequencies; unmeasured entries are marked `provisional`. A validator
  (`tools/gen_syscall_inventory.py`) fails CI on any missing, duplicated,
  misnumbered or mis-tiered entry, so the inventory cannot drift from the pinned
  kernel table.
- ✅ **Guest syscall dispatch + first semantics.** `0F 05` decodes to
  `Opcode::kSyscall`; `SyscallHandler` (`include/hyperion/syscall.hpp`) receives
  the guest state and writes `rax`. Both tiers route to it (the translator exits
  the block with `BlockResult::kSyscall`; the dispatcher calls the handler).
  `write`(1), `exit`(60) and `exit_group`(231) are implemented and tested;
  every other number returns `-ENOSYS` and is counted in
  `RunResult::unimplemented_syscalls`. Guest pointers are validated against the
  mapped region (`-EFAULT`, never a host crash).
- ✅ **Guest address space + memory syscalls.** `GuestAddressSpace`
  (`include/hyperion/address_space.hpp`) models the guest's virtual memory as a
  set of regions with per-region permissions and page-granular checks, plus a
  heap window reserved at construction so `brk` never collides with `mmap`.
  `mmap`(9) (anonymous only), `mprotect`(10), `munmap`(11) and `brk`(12) are
  implemented over it and marked `implemented` in the inventory. A file-backed
  `mmap` returns `-ENOSYS` and is counted as unimplemented rather than mapped
  wrongly. Tested end-to-end: a guest program mmaps a page, stores through the
  returned pointer and the value is readable (Tier 0), and under Tier 1 the
  syscall is handled and the block stops honestly at the memory-operand store.
- ✅ **Guest descriptor table + `read`/`close`/`lseek`.** `MinimalSyscallHandler`
  owns a guest descriptor table (guest fd → host stream); `read`(0), `close`(3)
  and `lseek`(8) operate on it and are marked `implemented` in the inventory. A
  guest fd that is not in the table is `-EBADF`, so a guest integer can never
  name an arbitrary host descriptor. The table starts with fd 0/1/2 on the
  process's stdin/stdout/stderr, and `adopt()` attaches an already-open host
  stream under another guest fd (the handler never `fclose`s a borrowed stream).
  `read` returns a short count at end-of-stream, `-EFAULT` on a bad guest buffer;
  `lseek` returns `-ESPIPE` for a non-seekable stream and `-EINVAL` for a bad
  `whence`.
- ✅ **`openat`(257) + `fstat`(5) (fourth slice).** `openat` opens a host file on
  the guest's behalf and allocates the lowest free guest fd ≥ 3; it is limited to
  `AT_FDCWD` and a bounded guest pathname, and an unhandled `O_*` flag or a real
  `dirfd` returns `-ENOSYS` and counts as unimplemented. The opened stream is
  handler-owned and released by `close`; `adopt()`ed streams stay borrowed.
  `fstat` writes the **guest** (x86-64) `struct stat` layout field-by-field,
  because the host layout differs on AArch64. Path policy is explicit: guest
  paths are forwarded to the host kernel (D-025) — Hyperion is **not a sandbox**.
- ✅ **Process-identity getters, `getcwd`, `getrandom` (fifth slice).**
  `getpid`/`gettid`/`getuid`/`geteuid`/`getgid`/`getegid`/`getppid` report the
  host process's values (Hyperion is single-process, D-026); `getcwd`(79) and
  `getrandom`(318) copy into a validated guest buffer, `getrandom` accepting only
  the two defined `GRND_*` bits.
- ✅ **`newfstatat`(262) (sixth slice).** glibc's `stat`/`lstat`/`fstatat` entry
  point; `AT_FDCWD` only, `AT_SYMLINK_NOFOLLOW` selects `lstat`. Shares the single
  guest-`struct stat` builder with `fstat` (D-027).
- ✅ **`access`(21) + `readlink`(89) (seventh slice).** `access` forwards to the
  host with `mode` restricted to `F_OK`/`R_OK`/`W_OK`/`X_OK`; `readlink` copies a
  symlink target with the kernel's truncation (not NUL-termination) semantics
  (D-028).
- ✅ **`readlinkat`(267) (eighth slice).** glibc's modern `readlink` entry point;
  shares the `readlink` body, `AT_FDCWD` only.
- ✅ **`getdents64`(217) (ninth slice).** Directory listing — the highest-frequency
  implementable syscall in the trace sample (3941 calls). The `linux_dirent64`
  record is a shared UAPI layout, so host entries are forwarded verbatim (D-030);
  `openat` now honours `O_DIRECTORY`/`O_NONBLOCK`/`O_CLOEXEC`, glibc's `opendir`
  flags.
- ✅ **`statx`(332) (tenth slice).** The modern, extensible stat (950 calls).
  `struct statx` is all fixed-width fields, so the layout is shared and the host's
  bytes are forwarded verbatim (D-030); `AT_FDCWD` only, `AT_SYMLINK_NOFOLLOW`
  the sole honoured flag.
- ⬜ The remaining U4 syscalls a program needs to start (`arch_prctl`,
  `set_tid_address`, `set_robust_list`, …) — **next**.
- ✅ **Guest process creation (twelfth slice).** `fork`(57), `vfork`(58), the
  process-creating subset of `clone`(56), `execve`(59) and `wait4`(61), in
  `src/core/process.cpp`. A guest child is a real host process running a deep
  copy of the `GuestAddressSpace`. Descriptor inheritance with a shared file
  offset, `FD_CLOEXEC` on `execve`, `pipe`/`pipe2` IPC, `PATH` search, and
  `CLONE_CHILD_SETTID`/`CLEARTID`/`PARENT_SETTID`/`SETTLS` are honoured.
  Verified end-to-end against a static x86-64 BusyBox shell pipeline on x86-64
  and on AArch64 under qemu, at both tiers.
- ✅ **Syscall compatibility engine (thirteenth–fifteenth slices).** Four-layer
  dispatch (compiled rule table → static switch → legacy compat switch →
  `-ENOSYS`), 39 ABI adapters (`epoll_*`, `poll`/`ppoll`, `select`/`pselect6`,
  `timerfd_*`, `eventfd2`, `memfd_create`, `getrusage`, `times`, `itimerval`,
  `sched_*`, `membarrier`, filesystem metadata, …, each translating a structure
  whose layout or flag values differ between x86-64 and AArch64), and a
  persistent, validated JSON registry outside the install directory with atomic
  writes, schema migration, provenance and a CLI. `nanosleep`/`clock_nanosleep`
  were moved from stub to real implementations. `link`(86)/`symlink`(88) were
  added after a guest fixture showed `ln` failing under translation.
- ⬜ Thread creation (`clone` with `CLONE_VM`/`CLONE_THREAD`). Currently
  **refused with `-ENOSYS` on purpose**, not merely missing: guest pages are host
  heap storage inside `GuestAddressSpace`, so a shared guest address space would
  need that model to change first. Downgrading to a fork would hand the guest a
  private copy it believes is shared. `futex`(202) itself is implemented but not
  yet validated.
- ⬜ Guest signal *handler invocation* (`rt_sigaction` is a **stub** that reports
  success and installs nothing — it must either work or refuse), `sigaltstack`
  (stub, and Rust's stack-overflow handling checks that it took effect),
  `waitid`(247).
- ⬜ Sockets (`socket`/`connect`/`bind`/`listen`): needed by an editor that talks
  to a language server; a large, separable piece.
- ✅ **Terminal `ioctl` coverage** — `TCGETS`/`TCSETS`/`TCSETSW`/`TCSETSF`,
  `TIOCSWINSZ` and `FIONREAD` on top of the existing `TIOCGWINSZ`, so a TUI can
  put a terminal into raw mode and size its window. Verified on a real
  pseudo-terminal: natively, in a unit test, and through a guest fixture under
  `script(1)`.
- ⬜ Guest concurrency: no test runs syscalls concurrently with a registry
  `reload()`. The design (immutable snapshot + `shared_ptr` swap) is sound; the
  concurrency is unproven.

### Phase 6 next steps, in priority order

The order is set by what a terminal application (a TUI editor) actually executes,
not by syscall number:

1. `rt_sigaction` + `sigaltstack` — replace the stubs with either a real
   implementation or an explicit refusal.
2. **Real `futex` blocking**, which is coupled to thread creation: `futex` now
   performs the value comparison and refuses to block (it used to report a
   wake-up that never happened). Making it block requires a guest wait queue,
   which requires `clone` with `CLONE_VM`/`CLONE_THREAD` first.
3. `select`/`poll`/`epoll` are done; `signalfd`, `inotify_*` are the next
   event-source gaps a file-watching editor hits.
4. Sockets.
5. Remaining terminal ioctls (`TIOCGPGRP`/`TIOCSPGRP`) and `TIOCLINUX` are `-ENOTTY`.
6. ~~Re-run `scripts/dev.sh cross-test`~~ — done, and now part of the driver: the
   AArch64 tree passes 10/10 with the compat suite at both tiers and a hard
   assertion that tier 1 executed generated code rather than falling back.

**Exit criteria.** A clearly defined set of real Linux guest programs works
correctly. No claim of general compatibility.

---

## Phase 7 — Advanced CPU compatibility ⬜

**Objective.** Handle complex instruction families.

**Work items.** SIMD (SSE/AVX subsets), atomics, exceptions, memory ordering,
CPUID, multithreading, self-modifying code — prioritized by measured
application requirements. Maintain an unsupported-feature list.

**Exit criteria.** Every newly supported feature has dedicated correctness tests.

---

## Phase 8 — AI optimization laboratory ⬜

**Objective.** Automated, evidence-gated optimization search.

**Work items.** Workflow letting an AI propose isolated optimizations, build
candidates, run tests, benchmark against a fixed baseline, and submit verified
changes for review. The baseline is immutable; failed tests cannot be bypassed.

**Exit criteria.** The pipeline rejects incorrect and slower candidates and
preserves successful ones.

---

## Phase 9 — Real application compatibility ⬜

**Objective.** Progressively demanding real programs.

**Work items.** Integration suite; library forwarding; graphics integration;
desktop applications; PRoot/Android constraints. Prefer upstream components
where safer than reinventing complex functionality.

**Kernel-dependent workloads (added by the 2026-10-19 scope correction).** For
programs that need real kernel semantics (services, containers, `systemd`),
evaluate the kernel-as-process tier against the reference device:

1. Build the AArch64 UML port and confirm it runs under Android 10's kernel
   with no root and no KVM (resolves R1/R2).
2. Measure its boot cost and syscall-boundary cost. No performance claim
   without numbers.
3. Implement it **only if** steps 1-2 succeed, behind the syscall-layer
   interface, and never as a replacement for the user-mode path.

**Exit criteria.** Published compatibility matrix with reproducible results,
stating plainly which workloads the user-mode path cannot run and whether the
kernel-as-process tier is available on the target.

---

## Phase 10 — Stabilization and release ⬜

**Objective.** A defensible experimental release.

**Work items.** Audit correctness, memory safety, resource consumption,
isolation, performance, portability, build reproducibility, documentation.

**Exit criteria.** A versioned experimental release whose documented exit
criteria are met. The project is never declared "complete" because a session
ended.

---

## Working agreement

Every milestone follows the loop in `docs/agent-handoff.md`: read the state,
pick the smallest meaningful task, state acceptance criteria, implement only
that scope, build, test, fix, update docs, commit, push, record the next step.
