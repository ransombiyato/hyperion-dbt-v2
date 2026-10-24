# STATUS

**Last updated:** 2026-10-23
**Current phase:** Phase 5 (performance measurement and optimization) — active;
Phase 6 (Linux ABI, ELF64 loading, and syscall compatibility) — twelfth slice
implemented: **guest process creation** (`fork`/`vfork`/`clone` subset/`execve`/`wait4`)
**Repository state:** buildable, tested, and documented; native GCC/Clang,
ASan/UBSan, AArch64 cross-tests, syscall inventory, shellcheck, and pinned
clang-format 19.1.7 checks pass. **Two execution tiers exist:** the reference
interpreter with a 2048-slot epoch-keyed decoded-instruction cache (Tier 0) and
a W^X `ExecArena`
translation cache with hardware `NZCV` condition fusion, native self-loop
superblocks, 2-way inline block chaining, and `Memory::code_epoch()` cache
invalidation (Tier 1).** Static x86-64 Linux ELF64 executables (`ET_EXEC` and
static-PIE `ET_DYN` with a System V AMD64 `argc/argv/envp/auxv` initial stack)
and raw blobs inspect and execute via `hyperion info <exe>` and
`hyperion run <exe>`. Phase 6's syscall inventory is validated bidirectionally
against C++ handler code (`tools/gen_syscall_inventory.py --check`), with
**47 syscalls across twelve slices implemented** (`read`, `write`, `close`,
`fstat`, `lseek`, `mmap`, `mprotect`, `munmap`, `brk`, `pread64`, `pwrite64`,
`readv`, `writev`, `access`, `dup`, `dup2`, `getpid`, `exit`, `uname`, `fcntl`,
`getcwd`, `readlink`, `gettimeofday`, `getuid`, `getgid`, `geteuid`, `getegid`,
`getppid`, `arch_prctl`, `gettid`, `getdents64`, `set_tid_address`,
`clock_gettime`, `exit_group`, `openat`, `newfstatat`, `readlinkat`,
`set_robust_list`, `dup3`, `prlimit64`, `getrandom`, `statx`, `fork`, `vfork`,
`clone`, `execve`, `wait4`). Unimplemented
numbers return `-ENOSYS` explicitly. See `COMPATIBILITY.md` for exact ISA scope,
and its §5.05 for exactly which process semantics are honoured and which are
refused. **Programs that create subprocesses now run**, including shell pipelines
under a real static x86-64 BusyBox.

This is the first file a returning agent should read. It records exactly what
works, what was last verified, and what to do next.

> **Environment recovery (read this first after a container reset).** `/usr`
> does not persist. Run `scripts/toolcache.sh install-host` (cmake/ninja/
> shellcheck from committed debs, works offline from a fresh clone), then
> `scripts/toolcache.sh restore` (cross toolchain + host binutils libraries
> from `tools/cache/`), then rebuild and rerun the tests. Do **not** trust a surviving `build/`
> directory. Full details in `docs/agent-handoff.md` §2 and §7.

---

## 0e. Phase 6 progress (syscall inventory + first slice)

| Work item | State |
| --- | --- |
| 1. Seven-file syscall inventory (U1–U4, E1–E3) | **Done** — `docs/syscalls/`, generated from `inventory.tsv` |
| 2. Evidence-based tiering | **Done** — `scripts/trace-syscalls.sh` strace sweep; unmeasured entries marked `provisional` |
| 3. Automated completeness validation | **Done** — `tools/gen_syscall_inventory.py`; wired into ctest (`syscall.inventory_valid`) and CI |
| 4. Guest syscall dispatch + semantics (first slice) | **Done & tested** — `include/hyperion/syscall.hpp`, `src/core/syscall.cpp`; `syscall` decoded (`0F 05`); both tiers route to the handler |
| 5. `write`/`exit`/`exit_group` semantics | **Implemented** — status `implemented` in the inventory; `-EBADF`/`-EFAULT` on bad fd/pointer |
| 6. Explicit `-ENOSYS` for every other number | **Implemented** — never silently succeeds; counted in `RunResult::unimplemented_syscalls` |
| 7. Guest address space (`GuestAddressSpace`) | **Done & tested** — `include/hyperion/address_space.hpp`, `src/core/address_space.cpp`; per-region permissions, page-granular checks, heap reserved at construction |
| 8. `mmap`/`mprotect`/`munmap`/`brk` over the address space (second slice) | **Implemented** — anonymous `mmap` only; file-backed requests return `-ENOSYS` and count as unimplemented |
| 9. Guest descriptor table + `read`(0)/`close`(3)/`lseek`(8) (third slice) | **Implemented & tested** — `MinimalSyscallHandler::adopt()` attaches an already-open host stream under a guest fd |
| 10. `openat`(257)/`fstat`(5) (fourth slice) | **Implemented & tested** — `openat` limited to `AT_FDCWD` and a bounded guest pathname; opens are handler-owned and closed on `close`; `fstat` builds the *guest* (x86-64) `struct stat` field-by-field, so the host layout (AArch64) cannot leak through |
| 11. Process-identity getters + `getcwd`/`getrandom` (fifth slice) | **Implemented & tested** — `getpid`(39)/`gettid`(186)/`getuid`(102)/`geteuid`(107)/`getgid`(104)/`getegid`(108)/`getppid`(110) forward to the host process (Hyperion runs the guest in-process, so the guest sees the host ids); `getcwd`(79) copies the host working directory into a bounded guest buffer (`-ERANGE` when it does not fit, `-EFAULT` on a bad pointer); `getrandom`(318) fills a guest buffer from the host in bounded chunks, validating the two defined `GRND_*` flag bits |
| 12. `newfstatat`(262) (sixth slice) | **Implemented & tested** — glibc's `stat`/`lstat`/`fstatat` entry point. `AT_FDCWD` only; `AT_SYMLINK_NOFOLLOW` selects `lstat`, any other flag or a real `dirfd` is `-ENOSYS` and counted as unimplemented. Reuses the shared guest-`struct stat` builder extracted from `fstat`, so the ABI-sensitive layout is written in exactly one place |
| 13. `access`(21) + `readlink`(89) (seventh slice) | **Implemented & tested** — `access` forwards to the host with mode restricted to `F_OK`/`R_OK`/`W_OK`/`X_OK` (any other bit is `-EINVAL`); `readlink` copies a symlink target without a NUL terminator, truncating to `bufsiz` (not an error) and rejecting `bufsiz == 0` with `-EINVAL`. Both read the path with the shared bounded `read_guest_cstr` |
| 14. `readlinkat`(267) (eighth slice) | **Implemented & tested** — glibc's modern `readlink` entry point. Shares the `readlink` body (no NUL terminator, silent truncation, `bufsiz == 0` is `-EINVAL`); `AT_FDCWD` only, any other `dirfd` is `-ENOSYS` and counted as unimplemented |
| 15. `getdents64`(217) (ninth slice) | **Implemented & tested** — directory listing, the highest-frequency implementable syscall in the trace sample (3941 calls). The `linux_dirent64` record is a fixed UAPI layout identical on x86-64 and AArch64, so host entries are forwarded verbatim (D-030); the kernel's own `-EBADF`/`-ENOTDIR`/`-EINVAL` come through unchanged. `openat` now honours `O_DIRECTORY`/`O_NONBLOCK`/`O_CLOEXEC` (glibc's `opendir` flags) |
| 16. `statx`(332) (tenth slice) | **Implemented & tested** — the modern, extensible stat (950 calls). `struct statx` is built only from fixed-width `__u32`/`__u64` fields, so its layout is identical on x86-64 and AArch64 and the host's bytes are copied into the guest verbatim (the `linux_dirent64` reasoning, D-030); `AT_FDCWD` only, `AT_SYMLINK_NOFOLLOW` the sole honoured flag |
| 17. Static ELF64 loader + SysV AMD64 stack + unbuffered POSIX descriptors | **Implemented & tested** — `elf_loader.{hpp,cpp}` loads `ET_EXEC` and static-PIE `ET_DYN` with W^X `PT_LOAD` mappings, BSS zeroing, and 16-byte aligned `argc`/`argv`/`envp`/`auxv` (`AT_PHDR`/`AT_PHENT`/`AT_PHNUM`/`AT_PAGESZ`/`AT_ENTRY`/`AT_RANDOM`); `OpenFileDescription` replaces buffered `FILE*` fd entries |
| 18. Startup, TLS, clock, resource, dup/fcntl, and scatter/positional I/O (eleventh slice) | **Implemented & tested** — adds `pread64`(17), `pwrite64`(18), `readv`(19), `writev`(20), `dup`(32), `dup2`(33), `dup3`(292), `fcntl`(72), `uname`(63, always advertising `"x86_64"`), `gettimeofday`(96), `clock_gettime`(228), `arch_prctl`(158, `ARCH_SET_FS`/`ARCH_GET_FS`), `set_tid_address`(218), `set_robust_list`(273), and `prlimit64`(302); **42 implemented syscalls total** |
| 19. Guest process creation (twelfth slice) | **Implemented & tested** — adds `fork`(57), `vfork`(58), the process-creating subset of `clone`(56), `execve`(59) and `wait4`(61) in `src/core/process.cpp`; **47 implemented syscalls total**. A guest child is a real host process running a *deep copy* of the `GuestAddressSpace` (guest pages are host heap storage, so a bare host `fork` would leave both sides sharing vectors that a later `mmap` reallocates). Process clone now honors `child_stack`; thread-creating and namespace-creating flags return `-ENOSYS` rather than being downgraded. A vfork child releases its parent on successful guest exec (not merely host exec), and a forked child can re-exec its image through `/proc/self/exe`. Verified against static x86-64 BusyBox: `sh -c 'printf "9\\n2\\n5\\n1\\n" \| sort -n'` → `1 2 5 9`, and `sh -c 'echo benchmark \| wc -c'` → `10`, matching native output on x86-64 and under AArch64 QEMU at both tiers |
| 20. Two CLI/loader correctness fixes found by the new guest tests | **Fixed** — (a) `hyperion run` returned 0 even when the guest exited non-zero unless `--quiet` was given, so an exit-status gate was told a failing guest had succeeded; the guest's status is now always the process status. (b) `AT_EXECFN` was filled from `argv[0]` instead of the executable's absolute path, which made a statically linked glibc guest abort inside `_dl_get_origin()` before its `main` ran; it now carries the real path, and the initial `%rsp` is explicitly re-aligned to 16 bytes rather than relying on the string pushes leaving it aligned |
| 21. Multi-layer syscall dispatch + compatibility adapters | **Implemented & tested** — `include/hyperion/syscall_dispatch.hpp`, `src/core/syscall_dispatch.cpp`, `src/core/syscall_adapters.cpp`. Dispatch order is compiled rule table (`kMaxSyscall = 512`, one bounds check + one index) → static switch → legacy compat switch → `-ENOSYS`. 39 adapters translate ABI differences (packed 12-byte `epoll_event`, `pollfd`, `fd_set`, `rusage`, `tms`, `itimerval`, `timespec`, guest fd → host fd). No JSON parsing, file I/O, lock, allocation or string hashing on the hot path; `reload()` publishes an immutable `shared_ptr<CompiledRules>` snapshot |
| 22. Persistent external JSON registry | **Implemented & tested** — `include/hyperion/syscall_registry.hpp`, `src/core/syscall_registry.cpp`. Schema v2 (v1 migrated on read), resolved as `--syscall-registry` → `$HYPERION_SYSCALL_REGISTRY` → `$XDG_DATA_HOME` → `$HOME/.local/share`; atomic write (temp + fsync + rename + dir fsync) with a `.bak`, `0600` files and `0700` directories; missing/corrupt/newer-schema files never block startup and are never rewritten by a read |
| 23. Registry CLI, installer integration, observability | **Implemented & tested** — `hyperion syscall-registry --list/--validate/--json/--export-defaults/--write` (`--validate` exits 3 when a rule was rejected rather than reporting success), `scripts/hyperion` gained the subcommand, `install.sh` creates the registry outside the install dir only when absent and preserves user rules on upgrade and uninstall; per-layer counters distinguish handled / unsupported / translation failures and unique numbers from events |

**Implemented-syscall status (verified automatically by
`tools/gen_syscall_inventory.py --check`).** 128 numbers are answered
today — 123 implemented and 5 stubbed — of the 375 in the pinned kernel
table. The validator now cross-checks three sources in both directions:
`MinimalSyscallHandler::handle()`, `is_linux_compat_syscall()` (whose numbers carry
a `[stub]` or `[unsupported]` annotation that decides the expected status), and the
compatibility layer's adapter table and alias rules. A syscall cannot be counted
as implemented in the inventory without a handler somewhere, and a handler cannot
exist without the inventory saying so.

The 5 stubs are `madvise`(28), `prctl`(157), `fadvise64`(221),
`clock_getres`(229) and `futex`(202). They answer success without performing the
call, or — for `futex` — perform the value comparison and then refuse to block,
and are listed as `stubbed` rather than
`implemented` because claiming them would be a lie a roadmap could be built on. `rseq`(334) is
recognised only to be refused cleanly and stays `not-started`.

Anything not listed returns `-ENOSYS` explicitly and is counted. The full list is
generated into `docs/syscalls/U1..U4.md`; the machine-readable source is
`docs/syscalls/inventory.tsv`.

**Address-space facts (verified).** `GuestAddressSpace` is the guest's virtual
memory: a set of regions with `MemPerm` (read/write/exec) and page-granular
checks. The heap lives in a reserved `[heap_base, heap_end)` window created at
construction, so `brk` grows into pre-reserved space and never collides with
`mmap`. `map_anonymous` allocates upward; `set_brk` maps the pages a grow
touches. Every guest pointer is validated against a region before use; the guest
never sees a raw host pointer. `mmap` reads only `addr, length, prot, flags`:
for `MAP_ANONYMOUS` the kernel ignores `fd`/`offset`, and Hyperion matches that
rather than inventing a stricter rule.

**Inventory facts (verified).** The pinned table is Linux v6.12
(`arch/x86/entry/syscalls/syscall_64.tbl`), reduced to the 375 `common`/`64`
entries (numbers 0–462; 336–423 is the x32 range, which Hyperion does not
target). The validator proves the inventory has exactly those 375, one entry
each. Distribution: U4=39, U3=75, U2=32, U1=14, E1=8, E2=100, E3=107.

---

## 0d. Phase 5 progress (performance measurement and optimization)

| Work item | State |
| --- | --- |
| 1. Tier 0 vs Tier 1 benchmark harness | **Done** — `benchmarks/tier_bench.cpp`, `hyperion_tier_bench` |
| 2. Reproducible baseline recorded before optimising | **Done** — BENCHMARKS.md §4.2, `scripts/bench-tier-qemu.sh` |
| 3. Optimization 1: parity (PF) fast path | **Done & measured** — BENCHMARKS.md §4.2.1; −28.8% generated code for the fixed workload |
| 4. Optimization 2: dead `rip` store elimination | **Done & measured** — BENCHMARKS.md §4.2.2; −9.3% generated code |
| 5. Optimization 3: flag liveness (skip dead flags) | **Done & measured** — BENCHMARKS.md §4.2.3; −39.7% generated code for the fixed workload |
| 6. Optimization 4: direct-mapped dispatch | **Done & measured** — BENCHMARKS.md §4.2.4; 99.9985% dispatch hit rate |
| 7. Optimization 5: ALU immediate folding | **Done & measured** — BENCHMARKS.md §4.2.5; −2 words on the size-guard program |
| 8. Optimization 6: guest-register allocation | **Done & measured** — BENCHMARKS.md §4.2.6; −15% Tier 1 time / −1.9% code on the wide workload, with a profitability guard |
| 9. Optimization 7: `ExecArena` + 2-way inline block chaining + hardware `NZCV` fusion + self-loop superblocks + Tier 0 icache | **Done & measured** — BENCHMARKS.md §4.2.8; **11.19×** faster Tier 1 narrow (`3.18 ns/insn`, smaller `344 B` code), **11.16×** faster Tier 1 wide (`0.94 ns/insn`), **3.47× / 2.90×** faster native Tier 0 |
| 10. Multi-tier compilation | **Not started** |

**Baseline (must not be silently changed).** Fixed guest loop, 200,000
iterations = 600,002 retired guest instructions, median of 7 samples under
`qemu-aarch64`: Tier 0 = 491.9 ns/guest-ins, Tier 1 = 38.4 ns/guest-ins, ratio
0.078 (Tier 1 ~12.8× faster). This is a *relative* signal between Hyperion's own
tiers, not a hardware performance claim; both tiers are additionally translated
by qemu. Every optimisation below is judged against this, and a candidate that
fails a correctness test is rejected regardless of speed.

**Optimization 1 (parity fast path) — accepted.** `emit_parity` replaced its
per-bit accumulation (19 emitted AArch64 instructions, including setup) with a
shifted-`EOR` XOR-fold (6). Verified by an interleaved A/B against the baseline commit on the
same host: generated code for the fixed workload fell 904 → 644 bytes (−28.8%,
deterministic) and Tier 1 wall time fell ~17.5%. Correctness was checked by the
full differential suite under `qemu-aarch64` (99 passed, 0 failed). The
deterministic size is now a CI regression guard
(`translator.translated_code_size_is_bounded`) because timing noise under
`qemu-user` would hide a code-size regression.

**Optimization 2 (dead `rip` stores) — accepted.** Nothing inside a translated
block reads `GuestState.rip`, so the per-instruction `movz`+`movk`+`str` that
advanced it was dead; the block now writes `rip` once at its exit. Generated
code fell 644 → 584 bytes (−9.3%, fully accounted for: 5 body instructions ×
3 instructions each) and Tier 1 wall time fell ~4% in an interleaved A/B
(BENCHMARKS.md §4.2.2). The differential suite compares the final `rip`, so a
misplaced write fails hard.

**Optimization 3 (flag liveness) — accepted.** Every ALU instruction emitted the
full six-flag prologue regardless of whether anything read the flags.
`translate_block` is now decode → backward flag liveness → emit, so an
instruction computes only the flags that are read before being overwritten
(`live_out` starts at all flags at the block exit, since flags persist across
blocks and there is no cross-block analysis). INC/DEC also lost their explicit
CF save/restore, because `flags_written()` excludes CF for them. Generated code
for the fixed workload fell 584 → 352 bytes (−39.7%, deterministic) and the
size-guard program fell 148 → 90 words; the `qemu-user` wall-clock change was
~0% and is **not claimed** as a speedup (BENCHMARKS.md §4.2.3). New guard:
`translator.dead_flags_are_not_computed`.

**Optimization 4 (direct-mapped dispatch) — accepted.** The dispatcher resolved
every next PC through `std::unordered_map`. A 4096-entry direct-mapped table
(low 12 bits of the PC) now answers the lookup, falling back to the map only on
a slot mismatch; the map stays authoritative, so a collision is a safe
fall-through, never a wrong block. Measured hit rate on the fixed workload:
**199,998 hits / 3 misses (99.9985%)** (BENCHMARKS.md §4.2.4). New test:
`translation_cache.direct_map_collision_falls_back_to_map`.

**Optimization 5 (ALU immediate folding) — accepted.** `add/sub/cmp reg, imm`
with an immediate that fits the AArch64 12-bit ADD/SUB immediate form now folds
into a single ADDS/SUBS-immediate, instead of materialising the constant into a
register first. The fold is applied only when AF is dead (AF needs `b` in a
register); a first unconditional attempt was caught by the differential test and
narrowed. Size-guard program fell 90 → 88 words (BENCHMARKS.md §4.2.5). New
test: `translator.alu_immediate_is_folded`.

**Optimization 6 (guest-register allocation) — accepted.** A pass now keeps the
block's most-used guest registers in callee-saved host registers (x19..x28) for
the block's duration, saving/restoring them around the block; `mov`, the ALU ops
and `inc`/`dec` operate directly on the allocated register when both operands are
allocated. The first version allocated *every* touched register and **regressed**
the tiny benchmark loop (364 → 388 bytes); it was replaced by a *profitability
guard* — allocate only when a register's body memory-op count exceeds the step
increase in prologue/epilogue overhead — which declines the 3-instruction loop
and fires on larger blocks. Because the narrow loop cannot show the win, a
`--wide` workload (12-instruction body, 11 guest registers) was added to the
harness: it measures **608 vs 620 bytes and 9.05 vs 10.70 ns/guest-instruction
(RA on vs off)**, reproducible across runs (BENCHMARKS.md §4.2.6). Adding the
many-register differential test also **exposed a pre-existing `rip` bug** (a
block ending in `HLT` never wrote `rip`, so the halted `rip` was the block start
instead of the `HLT` address); fixed and documented in BENCHMARKS.md §4.2.7. New
tests: `translator.differential_register_allocation_many_registers`,
`translator.differential_register_allocation_immediates`.

---

## 0c. Phase 4 progress (basic dynamic binary translator)

| Work item | State |
| --- | --- |
| 1. Basic-block formation (decode to a terminator) | **Done** — `translate_block` |
| 2. Block exit reporting (`BlockExit`, `BlockResult`) | **Done** — `include/hyperion/translator.hpp` |
| 3. Control-flow lowering (`jmp` rel32/rel8, `jcc` rel8) | **Done** — writes authoritative `rip` |
| 4. Translation cache + dispatcher | **Done** — `include/hyperion/translation_cache.hpp`, `src/core/translation_cache.cpp` |
| 5. `hyperion run --tier=0|1` CLI | **Done** — `src/cli/main.cpp` |
| 6. Whole-program differential tests | **Done** — loops and branches vs `run_program` |
| 7. Block chaining / hot-path optimization | **Not started** — Phase 5 |
| 8. Cache invalidation (self-modifying code) | **Not started** — the cache never invalidates; documented boundary |
| 9. Guest stack (`push`/`pop`/`ret`) | **Refused** — needs a guest address space (Phase 6) |

**What "done" means here, precisely.** `translate_block` decodes from a guest
PC until it reaches a terminator (`jmp`/`jcc`/`hlt`), the end of the supplied
buffer, or an instruction it cannot translate. It returns the generated code,
the exit kind, a fall-through/target hint, and the number of guest instructions
the block covers. The authoritative next PC is always the `rip` field that the
generated block writes into `GuestState`; the cache is only a lookup
accelerator.

`TranslationCache::run` is the dispatcher: it looks up the block for
`state.rip`, translates it on first sight, executes it, and repeats until the
program halts, an unsupported instruction is reached, or a guest-instruction
step limit is hit. It never executes an instruction the translator refused.

**Three real bugs were found by testing this increment and fixed:**

1. **Infinite loop on an unsupported first instruction.** A block whose *first*
   instruction is unsupported was emitted as a zero-instruction fall-through
   block, so the dispatcher re-entered the same PC forever. `translate_block`
   now reports `kUnsupportedOpcode` when the block would be empty. (This is the
   "never silently execute incorrect code" rule, and the reason the block cutter
   distinguishes "cut before an unsupported instruction" from "the first
   instruction is unsupported".)
2. **Short program images failed to translate.** The dispatcher's code-fetch
   window shrank only down to 15 bytes, so a 13-byte blob (the CLI smoke
   program) could not be read at all. The window now shrinks to a single byte.
3. **The dispatcher could exceed the step limit.** It checked the limit *before*
   running a block and then added the whole block's instruction count, so a run
   limited to 1,000,000 instructions could retire 1,000,001 (found by the
   nested-loop differential, which compared against the interpreter). A block is
   indivisible, so the dispatcher now refuses to *start* a block that does not
   fit in the remaining budget; a step-limited run may stop a few instructions
   short of the limit but never exceeds it.

**Decoder addition.** `0xEB` (`jmp rel8`) was added — a very common real-world
encoding and the natural companion to the `0x70..0x7F` short `jcc`. It has a
decoder test (`decoder.jmp_short_rel8`).

**Why the guest subset is still small.** Phase 4's exit criterion is that a
repeatable suite of small guest programs runs correctly under the translator,
not that the instruction set is large. The subset is unchanged from Phase 3
plus control flow; `push`/`pop`/`ret` and memory operands stay refused because
they need a guest address space (Phase 6).

**Guest program suite.** Five whole programs run through the dispatcher and are
compared against the interpreter: a single counted loop, taken/not-taken
branches across all flag inputs, **nested loops** (two backward branches), a
**branch inside a loop** whose taken and not-taken exits reconverge and whose
direction alternates each iteration, and an infinite loop (reported as a step
limit, not a hang). Two of them also assert an independently computed final
value, so a case cannot pass by the interpreter and the translator agreeing on
the same mistake.

---

## 0a. Phase 2 progress (reference interpreter and instruction subset)

| Work item | State |
| --- | --- |
| 1. Guest-state representation (GPRs, RFLAGS) | **Done** — `include/hyperion/guest_state.hpp` |
| 2. x86-64 decoder for a documented subset | **Done** — `include/hyperion/decoder.hpp`, `src/core/decoder.cpp` |
| 3. Reference interpreter (correctness oracle) | **Done** — `src/core/interpreter.cpp` |
| 4. Unit tests: decoder, interpreter, guest state | **Done** — 42 new cases (see §2) |
| 5. Differential test vs. real x86-64 hardware | **Done** — `tests/differential/`, 8 cases |
| 6. Documented instruction subset in COMPATIBILITY.md | **Done** — §2 is the Phase 3 contract |
| 7. Branch/loop execution over a program image | **Done** — `run_program()` + loop tests |

**Instruction subset (Phase 2).** MOV, ADD, SUB, AND, OR, XOR, CMP, INC, DEC,
PUSH, POP, JMP (rel32), Jcc (rel8), NOP, HLT, and RET-as-stop, at 32- and
64-bit operand sizes, with REX/ModRM/SIB addressing. **The authoritative
specification is `COMPATIBILITY.md` §2**, which also lists the planned-but-
unbuilt instructions separately. Everything else is explicitly rejected with a
specific `DecodeError`; the decoder never guesses.

**`run_program()`.** A convenience layer over `step()` that runs a program
image until HLT/RET, an error, or a step limit. It adds no new semantics, but
it makes multi-instruction programs testable — `test_interpreter.cpp` runs a
real `dec`/`jnz` loop (sum 1..10 = 55) and cross-checks it against native
x86-64, and asserts that an infinite loop reports a step limit rather than
hanging.

**Two real bugs were found by the differential test and fixed** (this is the
whole point of having one):
1. 64-bit ADD carry was never set — `sum > mask` is impossible when the mask is
   `~0`. Corrected to `(sum < ua) || (sum > mask)`.
2. `Jcc` short jumps (`0x70..0x7F`) were decoded as rel32 instead of rel8,
   producing wild jump targets.

**Integrity fix.** The test harness previously had no way for a test to report
"not executed", so the hardware-differential cases would have reported *PASS*
on AArch64 while comparing nothing. A `HYPERION_SKIP` mechanism was added; on
AArch64 the differential suite now honestly reports **0 passed, 8 skipped**.

## 0b. Phase 3 progress (first native code generation)

| Work item | State |
| --- | --- |
| 1. AArch64 encoder/emitter | **Done** — `a64::Encoder`, 18 reference-verified cases |
| 2. Executable memory (W^X + I-cache clear) | **Done** — `ExecMem`, 3 cases |
| 3. Run generated AArch64 code end to end | **Done** — `add` + summation loop pass under qemu |
| 4. Guest-state marshalling for generated code | **Done (increment 1)** — block signature `BlockResult block(GuestState*)` |
| 5. x86-64 → AArch64 instruction translation | **Done for the register/immediate subset** |

**What "done" means here, precisely.** `a64::Encoder` turns a small set of
AArch64 operations into 32-bit words, and every encoding is checked against the
output of GNU `as`/`objdump` rather than written from memory. `ExecMem`
allocates read-write, is filled, then becomes read-execute (W^X) with an
instruction-cache clear — see `DECISIONS.md` D-015, D-016.

`translate_instruction` (`include/hyperion/translator.hpp`) now lowers a decoded
x86-64 instruction to a self-contained AArch64 block. Supported today: `mov`,
`add`, `sub`, `and`, `or`, `xor`, `cmp`, `inc`, `dec`, `nop`, `hlt`, with 32/64-bit
register or immediate operands. Guest registers stay in the `GuestState` struct
in memory; the generated block loads and stores them (no register allocation
yet). Every one of the six x86 flags is *computed in generated code* from AArch64
NZCV plus two ALU identities, never delegated to the interpreter (D-017).

`tests/unit/test_translator.cpp` runs the generated blocks under `qemu-user` on
AArch64 and compares every register and all six flags against the interpreter —
a fixed instruction set plus an 847-case operand sweep over carry/overflow/parity
boundaries. On x86-64 the differential cases report **skip**, not a false pass.

**Why Phase 3 is closed at this scope.** The Phase 3 exit criterion — decoded
x86-64 instructions execute correctly through generated AArch64 code, validated
under `qemu-user` — is met for the translated subset. What remains is
*program-level* translation (loops, branches, block exits, a translation cache),
which is exactly Phase 4. Control flow (`jmp`/`jcc`/`ret`), the stack
(`push`/`pop`) and memory operands are refused explicitly with a `TranslateError`
rather than miscompiled.


---

## 0. Phase 1 progress

| Work item | State |
| --- | --- |
| 1. Prior-art survey (FEX, Box64, QEMU TCG, decoders) | **Done** — `docs/prior-art.md` |
| 2. Licence / trade-off comparison | **Done** — `docs/prior-art.md` §2, §3, `THIRD_PARTY.md` |
| 3. Reuse-vs-build boundary recorded | **Done** — `DECISIONS.md` D-013 |
| 4. Reproducible benchmark harness | **Done** — `benchmarks/`, `include/hyperion/bench_stats.hpp` |
| 5. Baseline numbers recorded, unavailable marked unavailable | **Done** — `BENCHMARKS.md` §4 |
| 6. Cross-build the AArch64 UML port (optional) | **Attempted, blocked** — see below |
| — One-line installer + `hyperion` command | **Done** (extra, requested) — `install.sh` |

**Item 6 result.** The AArch64 UML port (`zalexdev/linux-um-arm64`, Linux
7.2.0-rc4) configures with `ARCH=um SUBARCH=arm64` but fails to compile. The
failure is **not** arch-specific: stock **x86-64** UML fails at the identical
header line on this host, so it is a host glibc/kernel-header incompatibility.
The port is therefore **plausible but unverified**, and no claim is made that it
works. Full transcript in `docs/prior-art.md` §6.

**Exit criteria status.** `THIRD_PARTY.md` and the baseline comparison are
committed; `benchmarks/` produces repeatable, attributable results; `BENCHMARKS.md`
documents methodology and measured baselines. Phase 1 exit criteria are met
except that item 6 (optional) could not be completed for an environmental
reason that is now documented.

---

## 1. Current state (one paragraph)

The repository is a working C++20/CMake foundation. It builds clean with
warnings-as-errors, has a dependency-free unit-test harness wired into CTest,
and has a **verified cross-architecture validation pipeline**: the test suite is
cross-compiled to AArch64 and executed under `qemu-user`. A host capability
probe measures (rather than assumes) architecture, page size, cache-line size
and W^X expectations, and a benchmark harness emits JSON with repeated-run
statistics and guest-execution metrics explicitly marked unavailable.

Phase 2 added the first executable core: a **guest-state model**, an **x86-64
decoder for a documented subset** that rejects everything else explicitly, and a
**reference interpreter** that is the correctness oracle. Correctness is anchored
by a **differential test that compares the interpreter against real x86-64
hardware** (8 cases, thousands of comparisons across operand values and flag
inputs).

Phase 3 added the first code generation: a **checked AArch64 encoder**
(binutils-verified), **W^X executable memory**, and a **translator** that lowers
a subset of decoded x86-64 instructions to generated AArch64. Generated blocks
run under `qemu-user` in CI and are checked bit-for-bit against the interpreter,
including all six x86 flags.

Phase 4 turns that into a **basic dynamic binary translator**: `translate_block`
lowers a whole basic block (decode to a terminator, lower `jmp`/`jcc`, write the
authoritative next `rip`), and a **translation cache / dispatcher** runs complete
guest programs block by block, keyed by guest PC. The `hyperion run --tier=0|1`
command runs a raw x86-64 blob through either tier, and the two tiers are
required to agree. Whole-program differentials (a counted loop, taken and
not-taken conditional branches) pass under `qemu-user`. There is still **no
block chaining and no cache invalidation**; guest-register allocation exists
**within a block** (Phase 5). The guest stack and memory operands are refused
explicitly rather than miscompiled, and the interpreter remains the only
*complete* execution path.

Phase 5 has six accepted, measured optimizations (parity fast path, dead-`rip`
elimination, flag liveness, direct-mapped dispatch, ALU immediate folding,
guest-register allocation), each judged on deterministic evidence as well as
timing (BENCHMARKS.md §4.2).

Phase 6 has ten syscall slices implemented in both tiers over a
`GuestAddressSpace`: `write`/`exit`/`exit_group`; `mmap`/`mprotect`/`munmap`/`brk`
(anonymous `mmap` only); a handler-owned guest descriptor table with
`read`/`openat`/`close`/`lseek`/`fstat`; `getcwd`/`getrandom`; the identity
getters (`getpid`/`gettid`/`getuid`/`geteuid`/`getgid`/`getegid`/`getppid`);
`newfstatat`; `access`/`readlink`; `readlinkat`; `getdents64`; and `statx` —
twenty-seven numbers in total.
`openat` is limited to `AT_FDCWD`; every other number returns `-ENOSYS` and is
counted, never silently ignored.

The project also installs with a single command:

```bash
curl -fsSL https://raw.githubusercontent.com/ransombiyato/hyperion-dbt/main/install.sh | bash
```

which builds, tests and installs a `hyperion` command. `hyperion run <file>`
executes a raw x86-64 code blob through the reference interpreter (`--tier=0`)
or the AArch64 translator (`--tier=1`). On a host that cannot execute generated
AArch64 code (an x86-64 development host), tier 1 refuses and says so rather
than pretending.

Two things changed in an earlier session, and both matter more than the code:

1. **Scope was corrected.** The Phase 0 plan treated kernel/system-mode
   emulation as an explicit non-goal. That capped the project's compatibility
   ceiling before anything was measured, and it has been withdrawn
   (`DECISIONS.md` D-011). Kernel-level compatibility is now in scope for
   *investigation*, not implementation.
2. **The kernel-as-process option was de-risked by measurement.** A real Linux
   kernel was booted as an unprivileged process with no root and no KVM, and
   `systemd` ran as PID 1 to `multi-user.target`. See
   `docs/architecture-feasibility.md`.

The primary architecture is unchanged: the **user-mode translator** is still the
main path, and Phases 2-5 still build it.

---

## 2. Last successful verification

Re-verified on **2026-10-24** after the syscall compatibility engine (multi-layer
dispatch, ABI adapters, persistent registry, tests, benchmark, docs). Host:
x86-64, Debian 13, kernel 6.1.158, GCC 14.2.0, CMake 4.4.4, Ninja 1.13.2, plus
`aarch64-linux-gnu-g++` 14.2.0 and the cached `qemu-aarch64-static` for the cross
tree., persistent registry, tests, benchmark, docs). Host:
x86-64, Debian 13, kernel 6.1.158, GCC 14.2.0, CMake 4.4.4, Ninja 1.13.2.
**AArch64 execution was not available on this host**, so Tier 1 and the qemu
cross tree were not re-run in this session — that is the one gap in the table
below, and it is a host limitation, not a passing claim:

| Suite | Command | Result (measured) |
| --- | --- | --- |
| Native unit tests | `./build/dev/tests/hyperion_unit_tests` | **311 passed, 0 failed, 24 skipped** (of 335) |
| Full CTest tree | `ctest --test-dir build/dev` | **16/16 passed** (unit, differential, 3 CLI, 2 guest, inventory, 7 bench) |
| ASan + UBSan | `cmake -B build/asan -DHYPERION_ENABLE_SANITIZERS=ON` + `ctest` | **16/16 passed**, no sanitizer report |
| Guest compat suite | `guest.syscall_compat` | 9/9 checks (27 guest-side ABI assertions) |
| Registry CLI suite | `cli.syscall_registry` | 11/11 checks |
| Syscall inventory | `python3 tools/gen_syscall_inventory.py --check` | OK (0 errors, 0 warnings) |
| Dispatch microbenchmark | `benchmarks/hyperion_syscall_bench --iterations 300000 --repeats 9` | see `TESTING.md` §6 |
| Cross (qemu-user) | `scripts/dev.sh cross-test` | **10/10 passed** (see below) |
| AArch64 unit tests | cross build + `qemu-aarch64-static` | **344 passed, 0 failed, 2 skipped** (of 346) |
| AArch64 syscall compat, both tiers | `cross.aarch64_tests` | 11/11 checks per tier |
| AArch64 tier-1 JIT evidence | compat guest at `--tier=1` | `tier 1: 1412 basic block(s) translated`, `failures 0`, 170 308 steps — same as tier 0 |

The cross tree was run with a cross compiler installed for this session
(`apt-get install g++-aarch64-linux-gnu`) against the repo-cached
`tools/cache/bin/qemu-aarch64-static`. It previously did **not** cover the new
compatibility layer; `tests/run_cross_tests.cmake` now runs the guest compat suite
at both tiers, asserts that tier 1 really translated and executed generated code
(and did not fall back), and runs the registry CLI under qemu.

The ASan run earned its keep immediately: it found a stack buffer overflow in the
`poll` adapter that no ordinary build could see (`get_u64_le(raw + 4)` reading
eight bytes out of an eight-byte `struct pollfd`, where the overrun stayed inside
the same stack frame). Fixed, along with the 16/64-bit reader helpers that made
the mistake possible.

Previous run, from a clean tree on **2026-10-19**, host: x86-64, Debian 13
(trixie), GCC 14.2.0, Clang 19.1.7, CMake 3.31.6, Ninja 1.12.1, qemu-aarch64
10.0.13, aarch64-linux-gnu-g++ 14.2.0. This run followed a container reset that
removed `/usr`; the toolchain was restored from `tools/vendor/host-debs/` and
`tools/cache/` (with an apt fallback for the cross compiler) before rebuilding:, host: x86-64, Debian 13
(trixie), GCC 14.2.0, Clang 19.1.7, CMake 3.31.6, Ninja 1.12.1, qemu-aarch64
10.0.13, aarch64-linux-gnu-g++ 14.2.0. This run followed a container reset that
removed `/usr`; the toolchain was restored from `tools/vendor/host-debs/` and
`tools/cache/` (with an apt fallback for the cross compiler) before rebuilding:

| Suite | Command | Result |
| --- | --- | --- |
| Native | `scripts/dev.sh test` | 10/10 ctest passed (187 unit cases, 0 failed, 13 skipped) |
| Cross (qemu-user) | `scripts/dev.sh cross-test` | 6/6 ctest passed (198 unit cases, 0 failed, 2 skipped) |
| Clang | `scripts/dev.sh clang-build` | 10/10 passed (181 unit cases, 0 failed) |
| ASan/UBSan | `scripts/dev.sh sanitize` | 10/10 passed (181 unit cases, 0 failed) |
| Formatting | `scripts/dev.sh format` | clean (clang-format 19.1.7) |
| Lint | `shellcheck scripts/*.sh tests/cli/*.sh` | clean |
| Syscall inventory | `python3 tools/gen_syscall_inventory.py --check` | OK (0 errors, 0 warnings) |

Re-verified after the Phase 6 tenth slice (`statx`): native **187/0**, 13
skipped (of 200); cross **204/0**, 2 skipped (of 206) under `qemu-user`;
ASan/UBSan **187/0**; clang-build **187/0**; format clean; shellcheck clean;
syscall inventory valid (`statx` now `implemented`).

Re-verified after the Phase 6 ninth slice (`getdents64`): native **181/0**,
13 skipped (of 194); cross **198/0**, 2 skipped (of 200) under `qemu-user`;
ASan/UBSan **181/0**; clang-build **181/0**; format clean; shellcheck clean;
syscall inventory valid (getdents64 now `implemented`).

Re-verified after the Phase 6 eighth slice (`readlinkat`): native **175/0**,
13 skipped (of 188); cross **192/0**, 2 skipped (of 194) under `qemu-user`;
ASan/UBSan **175/0**; clang-build **175/0**; format clean; shellcheck clean;
syscall inventory valid (readlinkat now `implemented`).

Re-verified after the decoder truncation-diagnostic fix: native **171/0**,
13 skipped (of 184); cross **188/0**, 2 skipped (of 190) under `qemu-user`;
ASan/UBSan **171/0**; clang-build **171/0**; format clean; shellcheck clean;
syscall inventory valid.

Re-verified after the `hyperion` wrapper fix: native **169/0**, 13 skipped
(of 182); cross **186/0**, 2 skipped (of 188) under `qemu-user`; ASan/UBSan
**169/0**; clang-build **169/0**; format clean; shellcheck clean (including the
new `tests/cli/wrapper_test.sh`); syscall inventory valid. The new CTest case
`cli.wrapper` runs in the native, clang and sanitizer suites and in the cross
tree, and guards that the installed command never again denies an implemented
feature or refuses `run`.

Re-verified after the Phase 6 increment 7
commit (`access` + `readlink`): native 163/0, cross 180/0
under `qemu-user`, ASan/UBSan 163/0, clang-build 163/0, format clean, shellcheck
clean, syscall inventory valid. Previously, after increment 6
(`newfstatat`): native 158/0, cross 175/0 under `qemu-user`, ASan/UBSan 158/0,
clang-build 158/0, format clean, shellcheck clean, syscall inventory valid.
Previously, after increment 5
(identity getters + `getcwd` + `getrandom`): native 151/0, cross 168/0
under `qemu-user`, ASan/UBSan 151/0, clang-build 151/0, format clean, shellcheck
clean, syscall inventory valid. Previously, after increment 4
(openat + fstat): native 145/0, cross 153/0 under `qemu-user`,
ASan/UBSan 9/9, clang-build 9/9, format clean, shellcheck clean,
syscall inventory valid. Previously, after increment 3
(guest descriptor table + `read`/`close`/`lseek`): native 136/0,
cross 153/0 under `qemu-user`, ASan/UBSan 9/9, syscall filter 30/30.
Previously, after increment 2 (guest address space + memory syscalls,
commit `454c546`): native 125/0, cross 140/0, ASan/UBSan 9/9, CI run
`39410940495` green.

> **CI history, and a mistake worth not repeating.** CI run `39413203456` on
> commit `f41768b` **failed** in `build & test (clang)` and `ASan/UBSan`:
> clang's `-Wsign-conversion` rejected `files[fd]` (a signed index reaching
> `vector::operator[]`). The local gcc build was clean because gcc does not
> enable `-Wsign-conversion` under `-Wconversion`, so the failure was invisible
> here. Fixed in commit `6da32ac`, which also added `-Wsign-conversion` to the
> project warning set — so **gcc now catches this class of bug too**, and a
> second latent instance in `src/core/translator.cpp` was found and fixed by
> that flag. `scripts/dev.sh clang-build` was added to reproduce the clang path
> locally. An earlier session also recorded a green CI run for a commit that had
> never actually been pushed; verify with `git ls-remote origin main` after
> pushing rather than trusting the local log.

> **Environment note (important for the next agent).** This environment does
> **not** persist `/usr` between sessions. A new container starts with *no*
> toolchain: no `cmake`, `ninja`, `clang`, `qemu-aarch64`, or AArch64 cross
> compiler. The `build/` directory from a previous session may still exist and
> look valid (including genuine AArch64 ELF binaries), but the tools to rebuild
> it will be missing.
>
> The **repository directory itself does persist**, so the emulator and cross
> toolchain are cached under `tools/cache/` (git-ignored). Recovery order:
> `scripts/toolcache.sh restore` (reinstates a cached AArch64 toolchain without a
> network), then `scripts/bootstrap.sh` only if a tool is still missing. The
> benchmark and cross-test scripts pick up `tools/cache/bin/qemu-aarch64-static`
> automatically. Do not report a test as passing because an artifact from a
> previous session is present - rerun it.

| Check | Command | Result |
| --- | --- | --- |
| Native build (`-Werror`) | `cmake --build build/dev` | PASS — no warnings |
| Native tests | `ctest --test-dir build/dev` | PASS — **9/9** entries (136 unit cases + 8 differential) |
| Differential vs. hardware | `./build/dev/tests/hyperion_differential_tests` | PASS — 8/8 compared against real x86-64 |
| CLI smoke (both tiers) | `ctest --test-dir build/dev -R cli.smoke` | PASS — tiers agree, `steps=17` |
| Syscall layer (decode/handler/interpreter) | `hyperion_unit_tests --filter=syscall` | PASS — **30/30** cases (write/exit, memory syscalls, fd syscalls, mmap end-to-end) |
| Guest address space | `hyperion_unit_tests --filter=address_space` | PASS — map/read/write, permission denial, brk growth, page-granular checks |
| Syscall inventory validator | `python3 tools/gen_syscall_inventory.py --check` | PASS — inventory complete, consistent, valid |
| Cross build | `cmake --build build/cross-test` | PASS — AArch64 binaries produced |
| Cross tests under qemu | `ctest --test-dir build/cross-test` | PASS — **5/5** (153 passed, 0 failed, 2 skipped unit cases) |
| Tier 1 syscall path (AArch64) | `translation_cache.tier1_syscall_write_matches_interpreter` under qemu | PASS — generated code returns `kSyscall`, dispatcher handles it |
| AArch64 differential | `qemu-aarch64 .../hyperion_differential_tests` | SKIP — 0 passed, **8 skipped** (no x86-64 oracle) |
| Translator blocks vs. interpreter (AArch64) | `translator.block_*` under qemu | PASS — block cuts at terminators/unsupported/buffer end |
| Whole-program differentials (AArch64) | `translator.differential_program_*` under qemu | PASS — counted loop, nested loops, branch-inside-loop all match `run_program` |
| Dispatcher limits (AArch64) | `translator.dispatcher_*` under qemu | PASS — stops at unsupported; step limit is a hard bound, does not hang |
| Translation cache (AArch64) | `translation_cache.caches_*` under qemu | PASS — caches blocks, agrees with interpreter, direct-map hit rate > miss rate |
| Dispatch collision safety (AArch64) | `translation_cache.direct_map_collision_falls_back_to_map` | PASS — two PCs in one slot both run correctly, no re-translation |
| Flag liveness (AArch64) | `translator.dead_flags_are_not_computed` | PASS — a dead-flag `add` costs ≤4 words, a live one ≥15 |
| ALU immediate folding (AArch64) | `translator.alu_immediate_is_folded` | PASS — foldable immediate is strictly smaller than a materialised one |
| Register allocation, many registers (AArch64) | `translator.differential_register_allocation_many_registers` | PASS — 11-register program matches the interpreter instruction-for-instruction, including `rip` |
| Register allocation, immediates (AArch64) | `translator.differential_register_allocation_immediates` | PASS — allocated destination written by `mov` imm then read |
| Guest address space (native) | `address_space.*` unit cases | PASS — map/read/write, permission denial, `brk` growth, page-granular checks |
| Memory syscalls (native) | `syscall.mmap_*`, `syscall.mprotect_*`, `syscall.munmap_*`, `syscall.brk_*` | PASS — anonymous mapping usable, file-backed → `-ENOSYS`, no-address-space → `-ENOSYS`, `-EINVAL` cases |
| mmap end-to-end (native, Tier 0) | `syscall.mmap_end_to_end_under_the_interpreter` | PASS — guest mmaps a page, stores a byte through the returned pointer, value readable |
| mmap end-to-end (AArch64, Tier 1) | `syscall.mmap_end_to_end_under_generated_code_stops_at_memory_operand` | PASS — mmap handled; block stops at the memory-operand store (register-only generator), never mis-executes |
| Register-allocation benchmark (AArch64) | `scripts/bench-tier-qemu.sh --wide --iterations 100000 --repeats 7` | PASS — 608 vs 620 bytes, 9.05 vs 10.70 ns/guest-ins (RA on vs off) |
| Tool cache recovery | `scripts/toolcache.sh restore` after removing the sysroot | PASS — cross compiler and sysroot restored, cross tests still 100% |
| CLI tier 1 executes generated code | `HYPERION_EMULATOR=qemu-aarch64 ... cli_smoke.sh` | PASS — **3 basic blocks translated**, `steps=17` |
| ASan/UBSan (clang) | `ctest --test-dir build/san` | PASS — 9/9 |
| Formatting | `clang-format --dry-run --Werror` | PASS — clean |
| Shell scripts | `shellcheck install.sh scripts/*` | PASS — clean |
| Installer (local) | `install.sh --src . --prefix DIR` | PASS — builds, tests, installs, uninstalls |
| Installer (curl) | `curl .../install.sh \| bash` | PASS — end-to-end from GitHub |
| UML feasibility (smoke) | `experiments/uml/run.sh smoke` | PASS — kernel boots unprivileged; all 6 namespace types OK |
| UML feasibility (systemd) | `experiments/uml/run.sh systemd` | PASS — systemd 257 as PID 1 → `multi-user.target` |

The native suite is 9 CTest entries: `unit.native` (one entry running **136**
unit test cases across 12 files), `differential.native_vs_interpreter` (**8**
cases compared against real x86-64 hardware), `syscall.inventory_valid`,
`cli.smoke`, `tier_bench.smoke`, `bench.smoke`, `bench.methodology`,
`bench.timing_not_eliminated` and `bench.rejects_single_sample`. The cross build
(`build/cross-test`) has 5 entries: `unit.native`, `cli.smoke`,
`differential.native_vs_interpreter`, `cross.aarch64_tests` (which runs **153**
unit cases under `qemu-user`) and `syscall.inventory_valid`.

### Remote verification (GitHub Actions)

CI run `39446173442` on `main` (commit `60114c7`, Phase 6 increment 4:
`openat` + `fstat`): **conclusion `success`** — all six jobs passed:
`clang-format`, `build & test (gcc)`, `build & test (clang)`, `ASan/UBSan`,
`AArch64 cross tests (qemu-user)` and `syscall inventory & shellcheck`
(1m18s total).

CI run `39410940495` on `main` (commit `454c546`, Phase 6 increment 2:
guest address space + memory syscalls): **conclusion `success`** — all six jobs
passed: `clang-format`, `build & test (clang)`, `syscall inventory & shellcheck`,
`ASan/UBSan`, `AArch64 cross tests (qemu-user)`, and `build & test (gcc)`. The
AArch64 job builds the address-space and memory-syscall tests for AArch64 and
runs them under `qemu-user`.

CI run `39403321275` on `main` (commit `04c200d`, Phase 5 optimization 6 +
tool cache): **conclusion `success`** — all six jobs passed: `clang-format` (7s),
`build & test (clang)` (22s), `syscall inventory & shellcheck` (10s),
`ASan/UBSan` (31s), `AArch64 cross tests (qemu-user)` (48s), and
`build & test (gcc)` (27s). The AArch64 job executes the new
register-allocation differential tests under `qemu-user`.

CI run `39392485238` on `main` (commit `c40f3fc`, Phase 5 optimizations 3–5):
**conclusion `success`** — all six jobs passed: `build & test (gcc)`,
`build & test (clang)`, `AArch64 cross tests (qemu-user)`, `clang-format`,
`ASan/UBSan`, and `syscall inventory & shellcheck`. The AArch64 job executes the
generated code for the new dispatch/folding/liveness tests under `qemu-user`.

CI run `39540523397` on `main` (commit `0073998`, Phase 6 seventh syscall slice:
`access` + `readlink`): **conclusion `success`**.

CI run `39539804128` on `main` (commit `b9255e1`, Phase 6 sixth syscall slice:
`newfstatat`): **conclusion `success`**.

CI run `39539140453` on `main` (commit `ae8df19`, Phase 6 fifth syscall slice:
identity getters + `getcwd` + `getrandom`): **conclusion `success`**.

CI run `39389064540` on `main` (commit `250a148`, Phase 6 first syscall slice):
**conclusion `success`**.

CI run `39294174801` on `main` (commit `2148492`, Phase 4 translator):
**conclusion `success`** — all five jobs passed: `ASan/UBSan` (3m4s),
`build & test (gcc)` (27s), `clang-format` (6s), `build & test (clang)` (34s),
`AArch64 cross tests (qemu-user)` (54s). The AArch64 job executes the translated
blocks under `qemu-user`, runs the whole-program differentials, and runs the CLI
smoke test with `HYPERION_EMULATOR`, so tier 1 is proven to translate and run
generated code in CI.

CI run `39281904991` on `main` (commit `f1feee1`, Phase 3 translator):
**conclusion `success`** — all five jobs passed: `AArch64 cross tests (qemu-user)`
(1m18s), `build & test (gcc)` (49s), `clang-format` (44s), `ASan/UBSan` (39s),
`build & test (clang)` (24s). The AArch64 job is the one that executes the
translated blocks and runs `translator.differential_*` under `qemu-user`.

CI run `39272332010` on `main` (commit `d676695`, Phase 3 encoder + exec
memory): **conclusion `success`** — all five jobs passed: `AArch64 cross tests
(qemu-user)` (42s), `ASan/UBSan` (31s), `clang-format` (6s), `build & test
(clang)` (27s), `build & test (gcc)` (26s). The AArch64 job is the one that
executes generated code (`exec_mem.runs_generated_*`) under `qemu-user`.

CI run `39269782167` on `main` (commit `049fb99`, Phase 2 complete):
**conclusion `success`** — all five jobs passed.

CI run `39268700143` on `main` (commit `6c70b87`, Phase 2): **conclusion
`success`** — all five jobs passed: `clang-format` (7s), `AArch64 cross tests
(qemu-user)` (41s), `ASan/UBSan` (30s), `build & test (clang)` (32s), `build &
test (gcc)` (40s).

CI run `39263916932` on `main` (commit `cadbc17`): **conclusion `success`** —
all five jobs passed: `clang-format` (8s), `build & test (gcc)` (22s),
`ASan/UBSan` (33s), `AArch64 cross tests (qemu-user)` (31s), `build & test
(clang)` (24s).

Earlier green runs on `main`: `39263212528` (installer, `708c831`),
`39261295636` (benchmark methodology, `fc5fd42`), `39250218783` (`3f15e48`),
`39244780989` (`460629a`).

One non-failing annotation appears on every job: `actions/checkout@v4` targets
Node.js 20, which the runner now forces onto Node.js 24. It is a deprecation
notice, not a failure; tracked as ENV-003 in `docs/known-issues.md`.

Unit tests cover: version string well-formedness, build-summary content,
host-probe architecture agreement with compile-time arch, plausible CPU/page-size
reporting, W^X assumption per architecture, report formatting, and benchmark
statistics (median, quartiles, population standard deviation, and a
stable-vs-noisy discrimination case).

---

## 3. Failing tests / open questions

- **Failing tests:** none.
- **Known defects:** BENCH-001 (a fabricated benchmark result) was found and
  fixed in an earlier session; see `docs/known-issues.md`.
- **Known limitations of the Phase 4 dispatcher:**
  - The translation cache **never invalidates**. Self-modifying guest code will
    run stale translations. This is a deliberate, documented boundary for the
    first version; invalidation is Phase 7 work.
  - Blocks are **not chained**: every block exit returns to the dispatcher, which
    does a hash lookup. This is correct but slow; chaining is Phase 5.
  - Guest code is fetched through a 4 KiB window and the block is cut at the
    window edge. Longer blocks are correct (they just split), not miscompiled.
- **Known limitations of the guest address space / memory syscalls:**
  - `mmap` is **anonymous private only**. A file-backed or `MAP_SHARED` request
    returns `-ENOSYS` and is counted in `unimplemented_syscalls`; it is never
    mapped as something else.
  - `mprotect`/`munmap` require a page-aligned, **fully mapped** range and reject
    a range that would split an existing region. The kernel unmaps/splits partial
    ranges; this increment does not, and says so rather than pretending to.
  - The heap is a fixed reserved window (`heap_capacity`, 1 MiB by default), so a
    program that wants a much larger `brk` will fail. Real programs mostly
    `mmap`, which is unbounded up to the allocator cursor.
  - There is no demand-zero fault delivery to the guest: the address space is
    fully backed in host memory when mapped, not lazily.
- **Known limitations of the guest descriptor table (`read`/`close`/`lseek`/`openat`):**
  - **`openat` exists but is `AT_FDCWD`-only.** A guest can open a host file by
    name through `openat(AT_FDCWD, path, …)`; a real directory descriptor is
    `-ENOSYS`. Legacy `open`(2) is deliberately *not* implemented (glibc calls
    `openat`), and the embedder can still grant a stream with `adopt()`.
  - **Path-based metadata is `newfstatat`(262) only.** `stat`(4)/`lstat`(6) and
    `statx`(332) are not separate entry points (glibc routes `stat`/`lstat`
    through `newfstatat`). `AT_SYMLINK_NOFOLLOW` selects `lstat` semantics;
    `AT_EMPTY_PATH` and other flags return `-ENOSYS` and count as unimplemented.
    `newfstatat` and `fstat` share one guest-`struct stat` builder, so the
    ABI-sensitive layout is written in exactly one place.
  - **Path queries `access`(21) and `readlink`(89) exist, but not their `at`
    variants.** `readlinkat`(267)/`faccessat`(269)/`faccessat2`(439) are not
    implemented. `readlink` truncates to `bufsiz` (not an error) and never
    NUL-terminates, matching the kernel; `access` forwards the host process's
    permissions, so it is not a security boundary.
  - The table maps guest fd → `FILE*` (a `stdio` stream), not a raw host fd, so
    the handler cannot express `O_APPEND`, `pread`/`pwrite`, `fcntl`, or `dup`.
    `O_NONBLOCK` is honoured as an `open` flag, but the stream still blocks on
    `read`. `openat` honours the `O_*` subset it documents; any other flag is
    `-ENOSYS` (counted, never silently dropped).
  - `close` drops the guest descriptor but does **not** `fclose` the borrowed
    host stream: the handler does not own it, the embedder does. It *does*
    `fclose` a stream it opened itself via `openat`. A guest that closes fd 3
    cannot thereby close a host resource it was only lent.
  - `read` copies in 4 KiB chunks and, if the guest destination faults mid-way,
    returns `-EFAULT` after the bytes already read are consumed from the stream
    (documented in `do_read`); a real kernel faults before reading.
  - A guest fd that is not in the table is `-EBADF`, never a host descriptor.
- **Known limitation of the identity getters:** `getpid`/`gettid`/`getuid`/…
  return the *host* process's values, because Hyperion runs the guest in-process
  rather than as a separate guest kernel. For a single-process guest this is
  indistinguishable from the truth; `gettid` in particular assumes single-threaded
  execution and must be revisited when `clone` lands.
- **Known limitation of Tier 1 (the code generator):** it is **register-only**.
  Any instruction with a memory operand is rejected (`translator.cpp
  validate_body`) and the block stops with `kUnsupported` rather than emitting
  wrong code. So Tier 1 cannot yet execute a program that loads or stores, even
  though Tier 0 (the interpreter) can and the syscall layer now supports
  `mmap`/`brk`. This is the single biggest gap between the tiers and the gating
  item for real programs; it is the "next" item in §4.
- **Open questions:**
  - The AArch64 UML port has **not** been built or run. Its build is blocked by
    a host toolchain problem that also blocks stock x86-64 UML. Not measured on
    the reference device either. See `docs/prior-art.md` §6 and
    `docs/architecture-feasibility.md` §9.
  - Whether Android 10's seccomp/SELinux policy permits the `ptrace`/mmap
    behaviour the kernel-as-process tier needs is unmeasured (R2).
  - No measurement exists on the reference device (Snapdragon 855, Android 10).
    Everything recorded so far is from the x86-64 development host, except the
    AArch64 code paths, which run under `qemu-user`, not on real ARM hardware.
  - Tier 1 vs Tier 0 **is** now measured (BENCHMARKS.md §4.2–4.2.5), but only
    under `qemu-user` and only on one fixed loop workload. It is a relative
    sanity signal, not a hardware performance claim, and it does not generalise
    to real applications.
- See `docs/known-issues.md` for limitations.

---

## 4. Immediate next task

**The syscall compatibility engine is in place** — four-layer dispatch, 40 ABI
adapters, an external validated registry, a CLI, an installer step and tests at
three levels (unit, CLI, real-guest). 130 of 375 syscall numbers are
answered.

The next work is ordered by what a real terminal application (the stated target:
a TUI editor like Fresh) hits, not by syscall number:

1. ~~`rt_sigaction`(13) and `sigaltstack`(131) are stubs~~ — **done.** Both now
   record and report the guest's own state: the 32-byte x86-64 `struct sigaction`
   per signal (1..64), and the 24-byte `stack_t`. `SIGKILL`/`SIGSTOP` are refused
   with `EINVAL`, `sigsetsize != 8` is `EINVAL`, bad pointers are `EFAULT`, and a
   stack below the kernel floor (2048 B) is `ENOMEM`. What is *not* done, and is
   now stated as such (`LIM-022`), is **delivering** a signal to a guest handler.
2. ~~`ioctl`(16) coverage~~ — **done**: `TCGETS`/`TCSETS`/`TCSETSW`/`TCSETSF`,
   `TIOCSWINSZ`, `FIONREAD`, with the x86-64/AArch64 `c_cc` difference in an
   adapter and a unit test per direction.
3. ~~Cross-architecture verification~~ — **done**: the cross tree passes 10/10,
   the AArch64 unit suite is 342/0/2, and the compat guest runs at tier 1 with
   1412 translated blocks and identical step counts. Keep it that way: the
   driver now fails if tier 1 falls back to the interpreter.
4. **`futex`(202) needs a real wait queue.** The value comparison is now
   honest (`-EAGAIN`/`-EFAULT`) and `WAKE` reports zero woken truthfully, but
   `FUTEX_WAIT` on a matching value returns `-ENOSYS`. A threaded guest needs
   actual blocking, which means a futex table keyed on host addresses plus a
   wake path — the largest remaining correctness item for running real programs.
5. **Sockets** (`socket`(41), `connect`(42), `bind`(49), `listen`(50), …) are
   `not-started`. A TUI editor that spawns a language server needs them, but
   they are a large, separable piece — do them after 1–2.
6. **A process-wide registry reload test.** `reload()` swaps an immutable
   snapshot and is unit-tested for consistency, but no test runs guest syscalls
   concurrently with a reload. The design is sound; the concurrency is unproven.

Rules that apply to every syscall added from here:

- Implement it or refuse it; never a stub that reports success. If a temporary
  stub is unavoidable it goes in `is_linux_compat_syscall()` with a `[stub]`
  annotation, and the inventory gate then *requires* its status to be `stubbed`.
- Validate every guest pointer against the address space before use; return
  `-EBADF`/`-EFAULT`/`-EINVAL`/`-ENOSYS` exactly as the kernel does.
- Add a unit test including the invalid-argument paths, and a line in the guest
  fixture if the syscall is reachable from a real program.
- Move the number in `docs/syscalls/inventory.tsv`, regenerate the docs
  (`python3 tools/gen_syscall_inventory.py`), and update the counts in this file
  and `README.md`. The gate fails if the source and the table disagree.
- If the syscall needs structure or flag translation, write an adapter and a
  registry rule — do not special-case it in `handle_unguarded()`.

Then, independent and larger (Phase 5):

- **Memory operands in the code generator.** The AArch64 generator is
  register-only: it rejects any instruction with a memory operand
  (`translator.cpp validate_body`). Every real program loads and stores, so this
  is the gating item for running anything nontrivial in Tier 1.
- **Block chaining.** Today every block exit returns to the dispatcher. Link a
  block's exit directly to its successor's generated code, keeping the dispatcher
  as the fallback for unlinked exits.

Rules for every Phase 5 experiment (from `ROADMAP.md` and BENCHMARKS.md §5):

1. Do not change the baseline to make a candidate look good.
2. A candidate that fails any correctness test is rejected, however fast.
3. Record the commit SHA, command, and JSON in BENCHMARKS.md.
4. Keep a known-good implementation; optimisations must be reversible.

**The interpreter stays.** It is the oracle. Do not weaken it to make the
translator look correct. Phase 5 exit criteria are in `ROADMAP.md`.

Optional Phase 1 leftovers remain if wanted (unblocking the UML build, a CI
performance-trend baseline), but neither blocks Phase 5.

---

## 5. Exact commands to reproduce this state

```bash
scripts/bootstrap.sh            # one-time toolchain install + repo tool cache (needs sudo)
scripts/toolcache.sh restore    # after a container reset: reinstate the cached toolchain
scripts/toolcache.sh status     # what is cached
scripts/dev.sh bootstrap        # configure
scripts/dev.sh build            # build
scripts/dev.sh test             # native tests
scripts/dev.sh cross-test       # AArch64 tests under qemu-user
scripts/dev.sh bench --iterations 1000000 --repeats 9 --json
scripts/bench-tier-qemu.sh --iterations 200000 --repeats 7        # Tier 0 vs Tier 1 (narrow loop)
scripts/bench-tier-qemu.sh --wide --iterations 100000 --repeats 7 # register-allocation workload

# installer (no root, installs into ~/.local by default)
curl -fsSL https://raw.githubusercontent.com/ransombiyato/hyperion-dbt/main/install.sh | bash
hyperion doctor && hyperion test && hyperion bench

experiments/uml/run.sh smoke    # UML feasibility: kernel boots unprivileged
experiments/uml/run.sh systemd  # UML feasibility: systemd as PID 1 (slow)
experiments/uml/run.sh clean    # remove generated images/logs
```

---

## 6. Environment notes for the next agent

- The **development host is x86-64**, not AArch64. Use the cross + qemu
  pipeline (`scripts/dev.sh cross-test`) to execute AArch64 code.
- **Nothing in `/usr` persists between sessions** - see the note in section 2.
  The repository directory *does* persist, so a tool cache lives at
  `tools/cache/` (git-ignored). Recover with `scripts/toolcache.sh restore`
  before concluding that something is broken; `scripts/bootstrap.sh` reinstalls
  and re-populates the cache.
- **Host build tools are committed**, not just cached: `cmake`, `ninja` and
  `shellcheck` ship as `.deb` files under `tools/vendor/host-debs/` (19 MB) and
  install with `scripts/toolcache.sh install-host`. This is the only recovery
  path that works from a fresh clone with no network. `clang`/`libLLVM` is
  deliberately excluded (124 MB, over GitHub's per-file limit) and is not
  required — `-Wsign-conversion` is now on for gcc as well, so the gcc build
  catches the bug class that previously needed clang. See
  `tools/vendor/host-debs/README.md`.
- **Verify a push landed** with `git ls-remote origin main`; the embedded
  `origin` credential can be stale (HTTP 403) and a push can look successful
  while the remote is unchanged. Use `GITHUB_PERSONAL_ACCESS_TOKEN` if it 403s.
- The reference **target** device (Snapdragon 855, Android 10) is not present in
  this environment. Target-device claims must be measured on real hardware
  before being asserted; see `docs/target-environment.md`.
- Passwordless `sudo` is available for package installs.
- **Branch policy (changed):** development happens directly on `main` at the
  maintainer's request. `phase-0-foundation` has been fast-forwarded into
  `main` and is now redundant. Do not open a PR per milestone unless asked.
- `experiments/uml/` is evidence-gathering for an architecture decision. It
  contains **no emulation code** and is not part of the translator.
