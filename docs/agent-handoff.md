# Agent handoff

> **🤖 OpenHands Merge Instructions:** Before doing anything else when merging `ransombiyato/hyperion-dbt-v2` into `ransombiyato/hyperion-dbt`, read **[`../OPENHANDS_MERGE_GUIDE.md`](../OPENHANDS_MERGE_GUIDE.md)** in full!

**Read this first if you are a new AI agent continuing Project HYPERION.**

This document tells you exactly where the project is, how to verify it, and
what to do next. It is updated at the end of every work session. Do not rely on
conversation history — the repository is the memory.

---

## 1. What HYPERION is

An experimental **x86-64 → AArch64 user-mode dynamic binary translator**.
Reference target: Qualcomm Snapdragon 855, ARM64 Android 10, 8 GB RAM, no root,
no KVM. Development often happens on x86-64 Linux hosts.

---

## 2. Current state in one paragraph

**Phases 0–4 are complete, and Phases 5–6 are substantially advanced: Tier 1 is
11.19× faster on narrow loops and 11.16× faster on wide loops (`0.94 ns/insn`
under `qemu-aarch64`), Tier 0 is 3.47× / 2.90× faster natively, static x86-64
ELF64 executables load with a full System V AMD64 stack, and 42 Linux syscalls
are implemented with automated CI drift checking.**

- **Decoder & ISA (`decoder.{hpp,cpp}`):** Supports `MOV`, `LEA`, `ADD`, `SUB`,
  `CMP`, `TEST`, `AND`, `OR`, `XOR`, `INC`, `DEC`, `PUSH`, `POP`, `JMP rel8/rel32`,
  `Jcc rel8/rel32` (`0F 80..8F`), `SYSCALL`, `NOP`, `HLT`, `RET`, including
  `[rip + disp32]`, `[disp32]` / `[index*scale + disp32]`, accumulator immediates
  (`0x05..0x3D`, `0xA9`), multi-REX handling, and the 15-byte `DecodeError::kTooLong`
  architectural limit.
- **Tier 0 (`interpreter.{hpp,cpp}`):** Correctness oracle validated directly
  against native x86-64 silicon (`tests/differential/`), accelerated with a
  2048-slot `Memory::code_epoch()`-keyed decoded-instruction cache.
- **Tier 1 (`aarch64_encoder`, `exec_mem`, `translator`, `translation_cache`):**
  Multi-page W^X `ExecArena`, intra-block `x19..x28` register allocation,
  hardware `NZCV` condition fusion across arithmetic & logical ops, native
  self-loop superblock compilation (`(x0, x1)` ABI returning `(BlockResult,
  runs_executed)`), 2-way inline C++ block chaining, native `LEA`/`TEST`/`Jcc rel32`
  translation, and automatic `code_epoch` invalidation when executable pages
  change.
- **Address space, ELF64 loader, & Syscalls (`address_space`, `elf_loader`,
  `syscall`):** Page-granular W^X + enforced `MemPerm::kExec` instruction fetch,
  sub-region `split_region_at()` `mprotect`/`munmap`, static `ET_EXEC` +
  static-PIE `ET_DYN` ELF64 loading (`hyperion info <exe>`, `hyperion run <exe>`),
  unbuffered refcounted `OpenFileDescription` descriptors, and **42 Linux
  syscalls** enforced bidirectionally against `docs/syscalls/inventory.tsv` by
  `tools/gen_syscall_inventory.py --check`.

**Two real defects were found and fixed in Phase 4**, both by testing: a block
whose *first* instruction was unsupported was emitted empty and made the
dispatcher loop forever (now reported as an error), and the code-fetch window
shrank only to 15 bytes so a 13-byte program could not be translated (now
shrinks to one byte).

**Scope was corrected on 2026-10-19.** Kernel/system-mode emulation is no longer
an explicit non-goal (`DECISIONS.md` D-011). Kernel-level compatibility is in
scope for *investigation*; the user-mode translator remains the primary path.
The kernel-as-process option was de-risked by measurement and by desk research
(`docs/architecture-feasibility.md`). **No kernel-level code exists or should be
written yet.**

**Two real defects were found and fixed in Phase 2**, both by the differential
test — 64-bit ADD carry was never set, and `Jcc` short jumps were decoded as
rel32. A third issue was an *integrity* problem, not a code bug: the harness
could not report a test as "not executed", so the hardware-differential cases
would have claimed PASS on AArch64 while comparing nothing. `HYPERION_SKIP` was
added; on AArch64 the suite now reports `0 passed, 8 skipped`. When a test
cannot run, say so — never let it look like coverage.

**A real defect was found and fixed in Phase 1.** BENCH-001: the benchmark
harness reported a fabricated result because `-O2` eliminated the timed
workload. Read `docs/known-issues.md` §"Benchmarking defects" before touching
`benchmarks/`. The lesson generalises: a measurement that looks too good is
usually broken, and this project requires evidence, not plausible numbers.

> **The environment does not persist.** A new session starts with no toolchain in
> `/usr`. The **repository directory does persist**, so a tool cache lives at
> `tools/cache/` (git-ignored): recover with `scripts/toolcache.sh restore`
> (offline) and only then `scripts/bootstrap.sh` if a tool is still missing. Do
> not treat a stale `build/` directory as evidence that tests pass — rerun them.
>
> **You are probably not on the machine the last agent used, and you may not be
> an OpenHands agent at all.** Two recovery paths exist, and neither needs the
> other's history:
>
> 1. `scripts/toolcache.sh restore` — the AArch64 cross toolchain and
>    `qemu-aarch64-static`, from the git-ignored `tools/cache/`. Only present if
>    the workspace directory carried over. Needed only for `cross-test`.
> 2. `scripts/toolcache.sh install-host` — `cmake`, `ninja` and `shellcheck`,
>    from the `.deb` files **committed** under `tools/vendor/host-debs/`. This
>    works from a fresh clone and needs no network for those three packages.
>
> If both are unavailable, `scripts/bootstrap.sh` installs everything from a
> package mirror (needs sudo and network). The gcc toolchain plus
> `scripts/dev.sh build && scripts/dev.sh test` is enough to make progress; the
> cross and clang paths are additional coverage, not prerequisites.

---

## 3. Verify the current state (do this first)

```bash
scripts/toolcache.sh install-host # fresh clone: cmake/ninja/shellcheck from committed debs
# after a container reset: cached cross toolchain + host binutils runtimes (offline)
scripts/toolcache.sh restore
scripts/bootstrap.sh        # full toolchain (sudo); skip if already installed
scripts/dev.sh bootstrap
scripts/dev.sh build
scripts/dev.sh test         # expect: all tests passed
scripts/dev.sh cross-test   # expect: all tests passed incl. cross.aarch64_tests
scripts/dev.sh clang-build  # expect: clean (needs clang; optional)
./build/dev/tests/hyperion_differential_tests   # expect: 8 passed, 0 skipped (x86-64)
./build/dev/hyperion version
./build/dev/hyperion probe
python3 tools/gen_syscall_inventory.py --check   # expect: OK, inventory valid

# optional: the installer path (no root, installs into a temp prefix)
bash install.sh --src . --prefix /tmp/hp --yes
/tmp/hp/bin/hyperion doctor
bash install.sh --uninstall --prefix /tmp/hp
```

If `test` or `cross-test` does not pass, **stop and fix that first**. Do not
build new features on a broken base.

Expected test names: `unit.native`, `cli.smoke`, `differential.native_vs_interpreter`,
`syscall.inventory_valid`, `tier_bench.smoke`, `bench.smoke`, `bench.methodology`,
`bench.timing_not_eliminated`, `bench.rejects_single_sample`, and (cross build
only) `cross.aarch64_tests`. The suite has 9 tests natively and 10 in the cross
build.

---

## 4. Read these, in order

1. `STATUS.md` — precise current state and the immediate next task.
2. `ROADMAP.md` — all phases, the scope correction, and acceptance criteria.
3. `ARCHITECTURE.md` — design and component boundaries.
4. `docs/architecture-feasibility.md` — why kernel-level scope was reopened,
   and the measured evidence.
5. `DECISIONS.md` — why things are the way they are (D-001 … D-013).
6. `CONTRIBUTING.md` — conventions and the milestone workflow.
7. `docs/known-issues.md` — current limitations, including BENCH-001.
8. `docs/prior-art.md` — the survey and the reuse-vs-build boundary.
9. `docs/syscalls/README.md` — the syscall inventory, its tiers and its
   evidence policy; then the seven generated files `U1.md`–`U4.md`, `E1.md`–`E3.md`.

---

## 5. Your next task

**Phases 0–4 are closed. Phase 5 is active (six optimizations accepted and
measured: parity fast path, dead-`rip` elimination, flag liveness, direct-mapped
dispatch, ALU immediate folding, guest-register allocation). Phase 6's syscall
inventory is complete and CI-validated, and **seven** syscall slices are
implemented and tested in both tiers: the first (`write`/`exit`/`exit_group`),
the second (the guest address space plus `mmap`/`mprotect`/`munmap`/`brk`),
the third (a guest descriptor table plus `read`/`close`/`lseek`),
the fourth (`openat`/`fstat`), the fifth (the process-identity getters plus
`getcwd`/`getrandom`), the sixth (`newfstatat`, glibc's stat entry point), and
the seventh (`access`/`readlink`), the eighth (`readlinkat`), the ninth
(`getdents64`), and the tenth (`statx`).
The next concrete step is the remaining U4/E1 syscalls a real program needs to
start (`arch_prctl`, `set_tid_address`, `rseq`),
then memory operands in the code generator.** Guest state, the
decoder, the reference interpreter, the subset contract (`COMPATIBILITY.md` §2),
the hardware differential, the AArch64 encoder, executable memory,
single-instruction translation, whole-block translation, the translation-cache
dispatcher, the guest address space, and the syscall dispatch layer all exist
and pass.

1. ~~Build an AArch64 encoder~~ **Done** (`a64::Encoder`). ~~Add executable-code
   management~~ **Done** (`ExecMem`). ~~Marshall `GuestState` and translate one
   instruction~~ **Done** (`translate_instruction`). ~~Translate a basic block~~ **Done**
   (`translate_block`). ~~Add a translation cache and dispatcher~~ **Done**
   (`TranslationCache`). Add encoder instructions as the translator needs them,
   each with a binutils-verified test.
2. ~~Close Phase 4 out~~ **Done.** Exit criterion met (nested loops, a branch
   inside a loop, and the earlier programs all match the interpreter), CI green,
   recorded in `STATUS.md` §2.
3. ~~Build the Tier-1-vs-Tier-0 timing harness and record a baseline~~ **Done**
   (`benchmarks/tier_bench.cpp`, `scripts/bench-tier-qemu.sh`, BENCHMARKS.md
   §4.2): Tier 1 = 38.4 ns/guest-ins vs Tier 0 = 491.9 ns/guest-ins (ratio
   0.078) on a fixed loop under `qemu-user`.
4. ~~Optimization 1: parity (PF) fast path~~ **Done & measured.** (BENCHMARKS.md
   §4.2.1; 904 → 644 bytes, Tier 1 wall time −17.5%.)
5. ~~Optimization 2: dead `rip` stores~~ **Done & measured.** (BENCHMARKS.md
   §4.2.2; 644 → 584 bytes, Tier 1 wall time ≈ −4%.)
5a. ~~Optimization 3: flag liveness~~ **Done & measured.** (BENCHMARKS.md
   §4.2.3; 584 → 352 bytes, −39.7%; size-guard 148 → 90 words. Guard:
   `translator.dead_flags_are_not_computed`.)
5b. ~~Optimization 4: direct-mapped dispatch~~ **Done & measured.** (BENCHMARKS.md
   §4.2.4; 99.9985% dispatch hit rate. Guard:
   `translation_cache.direct_map_collision_falls_back_to_map`.)
5c. ~~Optimization 5: ALU immediate folding~~ **Done & measured.** (BENCHMARKS.md
   §4.2.5; size-guard 90 → 88 words. Guard:
   `translator.alu_immediate_is_folded`.)
5d. ~~Optimization 6: guest-register allocation (within a block)~~ **Done &
   measured.** (BENCHMARKS.md §4.2.6; on the new `--wide` workload 620 → 608
   bytes and 10.70 → 9.05 ns/guest-ins, with a profitability guard that declines
   tiny blocks. Guards: `translator.differential_register_allocation_many_registers`,
   `translator.differential_register_allocation_immediates`.) This work also found
   and fixed a pre-existing halted-`rip` bug (BENCHMARKS.md §4.2.7).
5e. ~~Cache the emulator and cross toolchain in the repository~~ **Done.**
   `scripts/toolcache.sh` (`populate`/`restore`/`status`/`env`), because the
   container wipes `/usr` between sessions but the repo persists (DECISIONS.md
   D-022).
6. ~~Establish the seven-file syscall inventory~~ **Done.** `docs/syscalls/`
   holds `U1.md`–`U4.md`, `E1.md`–`E3.md`, generated from `inventory.tsv` and
   validated against the pinned Linux v6.12 table by
   `tools/gen_syscall_inventory.py` (ctest `syscall.inventory_valid`, CI job
   `inventory`). Tiers are evidence-backed via `scripts/trace-syscalls.sh`.
7. ~~Implement the `syscall` instruction and a minimal dispatch layer~~ **Done &
   tested.** `Opcode::kSyscall` (`0F 05`) is decoded; `SyscallHandler`
   (`include/hyperion/syscall.hpp`) receives the guest state and writes `rax`.
   Both tiers route to it (the translator exits with `BlockResult::kSyscall` and
   the dispatcher calls the handler). `write`(1), `exit`(60), `exit_group`(231)
   are implemented; every other number returns `-ENOSYS`, counted in
   `RunResult::unimplemented_syscalls`. 11 new tests, including a Tier 1
   differential under `qemu-user`.
8. ~~Build the guest address space.~~ **Done.** `GuestAddressSpace`
   (`include/hyperion/address_space.hpp`, `src/core/address_space.cpp`) models
   guest virtual memory as permissioned regions with page-granular checks and a
   reserved heap window; `mmap`(9) (anonymous only), `mprotect`(10), `munmap`(11)
   and `brk`(12) are implemented over it and marked `implemented` in the
   inventory. A file-backed `mmap` returns `-ENOSYS` **and** counts as
   unimplemented. Tested end-to-end (DECISIONS.md D-023).
9. ~~Build a guest descriptor table and `read`/`close`/`lseek`.~~ **Done &
   tested.** `MinimalSyscallHandler` owns a guest descriptor table (guest fd →
   host stream) seeded with fd 0/1/2; `adopt()` attaches an already-open host
   stream under another guest fd; a guest fd not in the table is `-EBADF`.
   `read`(0), `close`(3) and `lseek`(8) are implemented and marked `implemented`
   in the inventory. `open`/`openat` were deferred in this increment (D-024).
   11 new tests.
10. ~~Implement `openat`(257) and `fstat`(5).~~ **Done & tested.**
    `openat` is limited to `AT_FDCWD` and a bounded guest pathname; unhandled
    flags or a real `dirfd` return `-ENOSYS` and count as unimplemented. The
    opened stream is handler-owned (`GuestDescriptor::owned`) and released by
    `close`; `adopt()`ed streams stay borrowed. `fstat` writes the **guest**
    (x86-64) `struct stat` layout field-by-field, because the host layout differs
    on AArch64. Path policy is an explicit decision: forward to the host kernel
    (D-025) — Hyperion is not a sandbox. 10 new tests; `openat`/`fstat` marked
    `implemented` in the inventory.
11. ~~Implement the process-identity getters, `getcwd`(79) and `getrandom`(318).~~
    **Done & tested.** `getpid`(39)/`gettid`(186)/`getuid`(102)/`geteuid`(107)/
    `getgid`(104)/`getegid`(108)/`getppid`(110) forward to the host process
    (Hyperion runs the guest in-process, so the guest sees the host's ids;
    `gettid` assumes single-threaded execution). `getcwd` copies the host working
    directory into a bounded guest buffer (`-ERANGE`/`-EFAULT`/`-EINVAL` as
    getcwd(2) documents). `getrandom` fills a bounded guest buffer in 4 KiB
    chunks, accepting only the defined `GRND_*` bits. Five new tests; all nine
    numbers marked `implemented` in the inventory.
12. ~~Implement `newfstatat`(262).~~ **Done & tested.** glibc's
    `stat`/`lstat`/`fstatat` entry point. `AT_FDCWD` only; `AT_SYMLINK_NOFOLLOW`
    selects `lstat`, any other flag or a real `dirfd` is `-ENOSYS` and counted as
    unimplemented. The guest-`struct stat` writer was refactored out of `fstat`
    into a shared `write_guest_stat`, so the ABI-sensitive layout lives in one
    place. Seven new tests, including a dangling-symlink case proving
    follow-vs-nofollow. 22 of 375 implemented.
13. ~~Implement `access`(21) and `readlink`(89).~~ **Done & tested.** Both
    forward to the host over the shared bounded `read_guest_cstr`; `readlink`
    mirrors the kernel's no-terminator/truncate semantics and clamps a huge
    `bufsiz`. Five new tests; both numbers marked `implemented`.
14. ~~Implement `readlinkat`(267).~~ **Done & tested.** Shares the `readlink`
    body, `AT_FDCWD` only (25 of 375 implemented).
15. ~~Implement `getdents64`(217).~~ **Done & tested.** Directory listing, the
    highest-frequency implementable syscall in the trace sample. Host entries are
    forwarded verbatim (shared `linux_dirent64` layout, D-030); `openat` now
    honours `O_DIRECTORY`/`O_NONBLOCK`/`O_CLOEXEC` so `opendir` can open a
    directory. Six new tests (26 of 375 implemented).
16. ~~Implement `statx`(332).~~ **Done & tested.** `struct statx` is all
    fixed-width fields, so the host bytes are forwarded verbatim (D-030);
    `AT_FDCWD` only. Six new tests (27 of 375 implemented).
17. **Next (Phase 6): the remaining U4/E1 syscalls a real program needs to
    start.** `arch_prctl`(158) (FS/GS base — needed before glibc startup, and the
    one that needs a new `GuestState.fs_base` field), `set_tid_address`(218),
    `rseq`(334) and `prlimit64`(302). Reuse the `newfstatat`/`fstat` shared guest-`struct stat` builder
    where applicable. Validate every guest buffer against the address space
    (`-EFAULT`), return `-EBADF`/`-ENOENT`/`-ENOSYS` as the kernel does, move each
    number's `status` in `docs/syscalls/inventory.tsv` to `implemented`,
    regenerate (`python3 tools/gen_syscall_inventory.py`), add a focused test per
    call, and update STATUS.md §0e.
17. **Then (Phase 6/7): memory operands in the code generator.** The AArch64
    generator is register-only (`translator.cpp validate_body` rejects any
    memory operand), so Tier 1 stops honestly at the first load/store. Every
    real program loads and stores, so this is the gating item for running
    anything nontrivial in Tier 1, and it pairs naturally with `read`/`openat`.
18. **Also open (Phase 5): block chaining.** Today every block exit returns to
    the dispatcher for a lookup. Link a block's exit directly to its successor's
    generated code so a hot loop stays in generated code, guarded by the same
    authoritative-`rip` rule, with the dispatcher as the fallback for any
    unlinked exit. Require the whole differential suite to pass, then re-run
    `scripts/bench-tier-qemu.sh` (both workloads) and record before/after in
    BENCHMARKS.md with an accept/reject verdict. Never change the baseline to
    make a candidate look good. Add a deterministic guard for whatever the
    change should move (e.g. dispatcher lookups per run), since `qemu-user`
    timing is noisy (DECISIONS.md D-020).
17. **Keep the oracle.** Run each multi-instruction program through both the
    generated blocks and the interpreter's `run_program`, and require the final
    `GuestState` to match bit-for-bit. A mismatch must fail loudly; there is no
    tolerance for "close enough".
18. Everything runs on the real AArch64 path for free: the `cross.aarch64_tests`
    pipeline builds the tests for AArch64, runs them under `qemu-user`, and runs
    the CLI smoke test with `HYPERION_EMULATOR`, so generated code is exercised in
    CI without the target device.

**The interpreter stays.** It is the oracle. Do not weaken it to make the code
generator look correct.

Full Phase 5 and Phase 6 acceptance criteria are in `ROADMAP.md`; the concrete
first steps are in `STATUS.md` §4.

**Do not** write kernel-level code — the tier is unvalidated on the reference
device. **Do not** claim a syscall works until it has a test and its inventory
`status` reflects reality.

Optional Phase 1 leftovers, neither blocking Phase 5/6: unblocking the AArch64
UML build (a host glibc/kernel-header problem, see `docs/prior-art.md` §6), and a
first CI performance-trend baseline.

---

## 6. Hard rules

1. **The repository is the memory.** Commit coherent changes; update `STATUS.md`
   and this file before you stop.
2. **Correctness outranks speed.** Never fabricate results, tests, or
   compatibility claims. Mark unavailable measurements unavailable.
3. **Work in small, verified milestones.** State acceptance criteria before
   coding; implement only that scope; build and test; fix failures; then commit.
4. **Component boundaries are inviolable.** The decoder must not know AArch64;
   the code generator must not know x86-64; the interpreter stays clear; the CLI
   holds no translation logic.
5. **No root, no KVM.** The core translator must run unprivileged on the target.
6. **W^X is assumed on AArch64.** Write code, then make it executable, then
   flush the instruction cache.
7. **Do not claim a phase complete because code exists.** Only evidence counts.
8. **Do not overwrite unrelated work.** Never touch existing Arch/FEXDroid
   installations or other projects.
9. **Never push a failing build or red CI.**
10. **Keep the syscall layer behind an interface.** It must be possible to
    direct syscalls to the host kernel or to a user-space kernel without
    rewriting the translator (`DECISIONS.md` D-011).
11. **Do not claim target-device support from host measurements.** Snapdragon
    855 / Android 10 has never been tested in this environment.

---

## 7. Environment specifics for this workspace

- **Development host is x86-64** (Debian 13). You cannot natively run AArch64
  binaries here — use the cross + qemu pipeline.
- **Nothing in `/usr` persists between sessions.** A fresh container has no
  `cmake`, `ninja`, `clang`, `qemu-aarch64`, or AArch64 cross compiler, even if a
  `build/` directory survives. The repository directory *does* persist, so recover
  with `scripts/toolcache.sh restore` first, then `scripts/bootstrap.sh` only if a
  tool is still missing.
- **`cmake`, `ninja` and `shellcheck` are committed** under
  `tools/vendor/host-debs/` (19 MB of `.deb` files) and installed with
  `scripts/toolcache.sh install-host`. This is the recovery path that works from
  a **fresh clone** with no network. `clang` is *not* vendored — it needs a
  124 MB `libLLVM.so`, over GitHub's per-file limit — but it is not required to
  build or test; see `tools/vendor/host-debs/README.md`.
- **Pushing:** the credential embedded in the `origin` URL can be stale and
  return HTTP 403 even though the token looks present. `git remote -v` shows a
  URL with an embedded token; if `git push` 403s, push with the
  `GITHUB_PERSONAL_ACCESS_TOKEN` environment variable instead, e.g.
  `git push "https://${GITHUB_PERSONAL_ACCESS_TOKEN}@github.com/<owner>/<repo>.git" main`,
  or `git remote set-url origin` to that form. Do not commit the token.
- **Verify a push actually landed.** A push can appear to succeed while the
  remote is unchanged. After pushing, confirm with
  `git ls-remote origin main` (or `git status -sb`) that `origin/main` is at the
  commit you just made, and check the CI run for *that* SHA rather than assuming
  it is green. A previous session recorded a green CI run whose commit had never
  been pushed — do not repeat that.
- **`qemu-user` is a correctness oracle only**, never a performance model.
- **Passwordless `sudo`** is available for package installation.
- The reference Snapdragon 855 device is **not** present; target-device claims
  must be measured on real hardware (`docs/target-environment.md`).
- **Branch policy (changed 2026-10-19):** develop directly on `main`. The
  maintainer asked for this. `phase-0-foundation` was fast-forwarded into
  `main`; do not open a pull request per milestone unless asked.

---

## 8. Where things live

| Need | Location |
| --- | --- |
| Build config | `CMakeLists.txt`, `CMakePresets.json`, `cmake/` |
| Core code | `src/core/`, headers in `include/hyperion/` |
| CLI | `src/cli/main.cpp` |
| Tests | `tests/harness/`, `tests/unit/` |
| Cross-arch driver | `tests/run_cross_tests.cmake` |
| Benchmarks | `benchmarks/` |
| Scripts | `scripts/dev.sh`, `scripts/bootstrap.sh`, `scripts/toolcache.sh` |
| CI | `.github/workflows/ci.yml` |
| Feasibility experiment | `experiments/uml/` |
| Architecture analysis | `docs/architecture-feasibility.md` |

Future component directories (`src/decoder/`, `src/ir/`, `src/interp/`,
`src/codegen/`, `src/runtime/`, `src/syscall/`) are created when their phase
begins — their absence is a deliberate signal of what does not exist yet.

`experiments/` is **not** part of the translator. It holds evidence-gathering
for architecture decisions and must never be linked into the product.

---

## 9. Session-end checklist

Before stopping for any reason:

- [ ] Working tree builds; relevant tests pass.
- [ ] Coherent changes committed (not left uncommitted).
- [ ] Pushed if credentials and permissions allow; **never claim a push
      succeeded unless it actually did**.
- [ ] `STATUS.md` updated: current state, last successful test, next task.
- [ ] `docs/agent-handoff.md` (this file) updated.
- [ ] `CHANGELOG.md` updated for the milestone.
- [ ] Failing tests and unresolved questions recorded.
- [ ] The single best next action written down explicitly.
