# CHANGELOG

Meaningful changes by milestone. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versions are pre-release.

---

## [Unreleased] — Dynamic syscall translation and compatibility engine

### Added
- **Multi-layer syscall dispatch (`include/hyperion/syscall_dispatch.hpp`,
  `src/core/syscall_dispatch.cpp`).** `MinimalSyscallHandler::handle()` now runs
  four layers in a fixed order: a compiled rule table (one bounds check and one
  array index, `kMaxSyscall = 512`), then the existing static switch, then the
  existing legacy compat switch, then a refusal that is counted. No JSON parsing,
  file I/O, lock, allocation or string hashing happens on this path: the registry
  is compiled once into an immutable `CompiledRules` snapshot published through a
  `shared_ptr`, and dispatch only reads it. Nothing was rewritten: the existing
  handlers are unchanged and the static implementation always wins — a rule for a
  number HYPERION already implements is rejected at compile time with a
  diagnostic instead of silently shadowing tested code.
- **39 ABI adapters (`src/core/syscall_adapters.cpp`).** Each translates a real
  cross-architecture difference rather than copying bytes: the x86-64 **packed
  12-byte** `struct epoll_event` versus the host's 16-byte aligned one (converted
  per entry, never `memcpy`'d), `struct pollfd` write-back that restores *guest*
  descriptor numbers and preserves the input `events` field, `fd_set`
  reconstruction for `select`/`pselect6`, and `struct rusage`, `struct tms`,
  `struct itimerval` and `timespec` layouts. Adapters are the only way a registry
  rule can name an implementation: the JSON can select a compiled, tested
  function by id, never a host syscall number, a library, a shell command or an
  expression.
- **Persistent external JSON registry
  (`include/hyperion/syscall_registry.hpp`, `src/core/syscall_registry.cpp`).**
  Schema v2 (v1 migrated on read), stored *outside* the install directory at
  `$XDG_DATA_HOME/hyperion/syscalls/registry.json` or
  `$HOME/.local/share/hyperion/syscalls/registry.json`, overridable with
  `$HYPERION_SYSCALL_REGISTRY` or `--syscall-registry=FILE`. Rules carry an id,
  name, syscall number, implementation, handler, `origin` (`builtin`/`package`/
  `user`), `rule_version` and `enabled`; shipped defaults are merged by id so an
  upgrade can update them without touching user rules.
- **Safe persistence.** Every field is validated, a bad rule is rejected without
  clobbering the rest of the registry, writes are atomic (temp file + `fsync` +
  `rename` + directory `fsync`, previous contents kept as `.bak`, files `0600`,
  directories `0700`), reads are capped at 16 MiB, and a missing, unreadable,
  corrupt or newer-schema file leaves the built-in rules in force — it can never
  prevent startup, and reading never rewrites the file. The previously accepted
  "any schema_version up to 1000" check now rejects anything newer than the build
  supports and says so.
- **Real semantic translations.** `nanosleep`(35) and `clock_nanosleep`(230)
  were stubs that returned 0 immediately — which turned every guest retry or
  backoff loop into a spin. They now validate the request (`tv_nsec < 1e9`, no
  negative fields), perform the host call, report `-EINTR` with the unslept
  remainder written back for the relative form, and accept only `TIMER_ABSTIME`.
  `link`(86) and `symlink`(88) were missing entirely: the adapter layer had
  `linkat`/`symlinkat`, but glibc issues the plain calls on x86-64, so `ln` and
  `ln -s` failed under translation. Found by the new guest fixture, not by
  inspection.
- **Observability.** Per-layer counters (`adapter`, `alias`, `existing`,
  `unsupported`, `translation_failures`), a distinct unsupported-number set
  reported with its numeric and hex value, and a dispatch summary printed by
  `hyperion run`. `HYPERION_TRACE_SYSCALLS=1` already logged every call; the
  counters add the aggregate view. The long-standing `unimplemented syscalls: N`
  line is now documented as counting **events**, not distinct numbers, and as
  including refusals that a later layer then handled.
- **`hyperion syscall-registry` CLI** (`--list`, `--validate`, `--json`,
  `--export-defaults`, `--write`, `--registry=FILE`), plus `--no-syscall-registry`
  and `HYPERION_NO_SYSCALL_REGISTRY` on `run`. `scripts/hyperion` gained the
  subcommand (it previously printed `unknown command`).
- **Tests.** ~55 new unit cases across `test_json.cpp`,
  `test_syscall_registry.cpp`, `test_syscall_dispatch.cpp` and
  `test_syscall_adapters.cpp`; a real-guest suite
  (`tests/guest/syscall_compat_test.sh`, CTest `guest.syscall_compat`) that runs a
  statically linked x86-64 fixture with 27 behavioural checks and proves a
  user-written rule enables a syscall the binary has never heard of; a CLI
  contract suite (`tests/cli/registry_cli_test.sh`, CTest
  `cli.syscall_registry`); and a dispatch microbenchmark
  (`benchmarks/syscall_bench.cpp`).
- **Inventory accuracy.** `tools/gen_syscall_inventory.py` now also scans
  `is_linux_compat_syscall()` (with `[stub]`/`[unsupported]` annotations) and the
  adapter/alias tables, so the inventory cannot claim a syscall has no
  implementation while one exists, or vice versa. That correction alone moved 39
  rows from `not-started` to `implemented`/`stubbed`: the roadmap had been aimed
  at work that was already done.

### Fixed
- **`rt_sigaction` fabricated the guest's signal dispositions.** The syscall wrote
  32 zero bytes into the caller's `oldact` and returned 0 regardless of what the
  guest had installed, so a program that installed `SIGSEGV` and read it back was
  told `SIG_DFL`. Dispositions for signals 1..64 are now recorded and reported
  (handler, flags, restorer, mask), and the error paths match the kernel:
  `-EINVAL` for out-of-range signals, for `SIGKILL`/`SIGSTOP`, and for
  `sigsetsize != 8`; `-EFAULT` for either pointer outside the guest address space.
  Signal *delivery* is still not implemented and is now documented as its own
  limitation (`LIM-022`) instead of being implied by a stub.
- **`sigaltstack` did the same to the alternate signal stack.** It now stores and
  reports the 24-byte `stack_t`, reports `SS_DISABLE` before anything is
  installed, clears on `SS_DISABLE`, and returns `-EINVAL` for undefined flag bits
  and `-ENOMEM` below the kernel's 2048-byte floor. This is the read-back Rust's
  standard library performs to confirm its stack-overflow guard is armed.
- **`futex` reported wake-ups that never happened.** `FUTEX_WAIT` returned 0
  whenever the value matched, without blocking — telling the caller it had been
  woken when nothing had occurred. A guest thread cannot exist here (`clone` with
  `CLONE_VM`/`CLONE_THREAD` is refused), so no waiter could ever be released and
  blocking would hang. The call now does the real work it *can* do and refuses the
  rest: `-EAGAIN` when the word changed, `-EFAULT` on an unmapped address,
  `WAKE` reports zero woken (true), and the blocking/PI/requeue paths return
  `-ENOSYS`. Its inventory status was corrected from `implemented` to `stubbed`.
- **A terminal application could not enter raw mode.** `ioctl` handled
  `TIOCGWINSZ` only, so `cfmakeraw()` — the first thing every TUI does — failed
  and the application either exited or drove the terminal in cooked mode.
  `TCGETS`, `TCSETS`, `TCSETSW`, `TCSETSF`, `TIOCSWINSZ` and `FIONREAD` are now
  implemented over the 36-byte asm-generic `struct termios` (the layout shared by
  x86-64 and AArch64; glibc converts the 60-byte userspace struct before issuing
  the ioctl). Unknown requests return `-ENOTTY`, not `-ENOSYS`. Verified against a
  real pseudo-terminal in three places: natively, in
  `syscall.linux_compat_ioctl_tcgets_then_tcsets_round_trips`, and by the guest
  fixture run under `script(1)` in `guest.syscall_compat`.
- **Real registry rules could be silently dropped while `--validate` exited 0.**
  A rule for a syscall HYPERION implements statically, or with an unknown
  handler, was rejected during compilation and reported only as a warning. A user
  checking "is my rule in effect?" was told yes. `--validate` now exits `3` when
  the file parsed but at least one rule was rejected, and `1` when the file could
  not be used at all.
- **`--json` reported the file's own rules, not the effective set.** For a setup
  with 43 active rules it printed one, and said nothing about rejections. It now
  reports the resolved path and its source, the effective rule set, per-layer
  counts, rejected rules and diagnostics.
- **Every rejected/warning diagnostic was printed twice**, because
  `reload_syscall_registry` folded the dispatcher's diagnostics into its own
  one-line summary.
- **Stack buffer overflow in the `poll` adapter.** `get_u64_le(raw + 4)` read
  eight bytes out of the sixteen-bit `events` field of an eight-byte
  `struct pollfd`. The overrun stayed inside the same stack frame, so every
  ordinary build passed; AddressSanitizer caught it. Fixed, and `get_u16_le` /
  `get_u32_le` / `put_u16` added so a field can be read at its own width.
- **A forked child lost the parent's registry rules.** `src/core/process.cpp`
  built the child's handler from scratch, so it started with the built-in rules
  only: a user rule that worked before `fork(2)` returned `-ENOSYS` afterwards.
  The child now adopts the parent's immutable snapshot
  (`SyscallDispatcher::adopt_snapshot`), which is both correct and cheaper than
  re-reading the file. Proven end-to-end by `tests/guest/fixtures/fork_rule_inherit.c`
  — with the fix removed, the guest prints `parent=1 child=0`, so the test cannot
  pass vacuously.
- **`install.sh` ignored the registry.** It now creates the registry outside the
  install directory only when it is absent, validates (never overwrites) an
  existing one, and deliberately keeps user rules on `--uninstall`.

### Verification
- **The AArch64 Tier 1 path is now covered by the cross pipeline.**
  `tests/run_cross_tests.cmake` runs the new guest compat suite at both tiers
  against the AArch64-built binary, runs the registry CLI under qemu, and fails
  if tier 1 falls back to the interpreter. Measured with
  `g++-aarch64-linux-gnu` 14.2.0 and the cached `qemu-aarch64-static`:
  `scripts/dev.sh cross-test` **10/10**, AArch64 unit suite **347 passed / 0
  failed / 2 skipped** of 349, and the compat guest at `--tier=1` reports
  `1412 basic block(s) translated`, `failures 0` and **the same 170 308 guest
  steps as tier 0**. `tests/guest/syscall_compat_test.sh` and
  `tests/cli/registry_cli_test.sh` grew an optional emulator argument so both can
  be driven against a cross-built binary.

### Changed
- **`enable_linux_compat()`'s switch is documented as the third dispatch layer**
  and its number list is machine-read: `[stub]` marks a number that answers
  success without doing the work (the inventory must then say `stubbed`) and
  `[unsupported]` marks one that is only recognised to be refused cleanly
  (`rseq`), which stays `not-started`.
- **Tier 1 coverage is now part of the cross pipeline** rather than a manual step
  (see Verification above).

---

## [Unreleased] — Guest process creation (`fork`/`vfork`/`clone` subset/`execve`/`wait4`), installer repair, and documentation reconciliation

### Added
- **Guest process creation (`src/core/process.cpp`, `include/hyperion/process.hpp`):** `fork`(57), `vfork`(58), the process-creating subset of `clone`(56), `execve`(59) and `wait4`(61) — 42 → **47 implemented syscalls**. A guest child is a real host process that runs a **deep copy** of the `GuestAddressSpace` to completion and terminates with the guest's exit status; the parent records it and reaps it with `wait4`. Honouring `fork` correctly needs the copy, because guest pages are host heap storage owned by that object: a bare host `fork` would leave parent and child sharing the same `std::vector`s until either side `mmap`/`munmap`s and reallocates them, invalidating the other's cached host page pointers.
- **Supported process semantics:** child exit-status propagation, descriptor inheritance with a **shared file offset**, per-descriptor `FD_CLOEXEC` on `execve`, `pipe`/`pipe2` inter-process communication, `execve` `PATH` search for bare program names, `CLONE_CHILD_SETTID`/`CLONE_CHILD_CLEARTID`/`CLONE_PARENT_SETTID`/`CLONE_SETTLS`, `wait4` with `WNOHANG`/`WUNTRACED`/`WCONTINUED` and a guest-layout `struct rusage`, and `vfork` parent suspension until the child execs or exits.
- **`GuestAddressSpace::reset()` and a deep-copy constructor** (`address_space.{hpp,cpp}`): `execve` clears the outgoing image in place (the executor holds a reference to the object, so it cannot be replaced) and bumps `code_epoch()` so both tiers drop their translations of an image that no longer exists; `fork` copies.
- **`/proc/self/exe` redirection:** `readlink`/`readlinkat`/`execve` resolve it to the *guest's* image rather than the translator. A guest runs inside hyperion's host process, so the host's `/proc/self/exe` names hyperion — and BusyBox re-executes itself through exactly that path to run an applet.
- **`AT_EXECFN` now carries the executable's absolute path** (`elf_loader.{hpp,cpp}`, `cli/main.cpp`), matching what the kernel records, and the initial `%rsp` is explicitly re-aligned to 16 bytes instead of relying on the auxiliary-string pushes leaving it aligned.
- **`HYPERION_TRACE_SYSCALLS=1`** logs every guest syscall with its result and the host pid, written with `write(2)` so it cannot be swallowed by or corrupt the guest's own buffered output. Added because an unimplemented call was previously visible only as a count, which is not enough to diagnose a misbehaving guest.
- **Guest process regression suite (`tests/guest/`, CTest `guest.process`):** real compiled x86-64 guests, built by the test from C sources with `cc -O2 -static` and verified to be `EM_X86_64` before running. Covers fork + exit-status propagation, process-clone `child_stack`, `execve` of a second program, fd inheritance, pipe IPC, vfork + exec release through `/proc/self/exe`, the glibc `clone` error paths, the translator's deliberate `ENOSYS` refusals, and two static-x86-64 BusyBox shell pipelines. Every case asserts the guest's stdout **and** exit status, never the translator's own report. Registered only when a static-x86-64 fixture compiler exists, so a missing toolchain cannot look like a passing suite.
- **`hypy` launcher (`scripts/hypy`)** — the short front end for running a guest, installed alongside `hyperion`; `hypy <elf> [args...]`, `hypy run ...`, and `version`/`probe`/`info` pass-through.
- **`hyperion probe`** is now reachable through the `scripts/hyperion` wrapper.

### Fixed
- **`hyperion run` reported success for a failing guest.** The guest's exit status was honoured only under `--quiet`; without it a guest that exited non-zero still made hyperion exit `0`, so any gate built on the exit status was told a broken run had succeeded. The guest's status is now always the process status.
- **`hyperion run` consumed the guest's own options.** `hyperion run busybox sh -c 'a | b'` failed with `unknown command '-c'`, because hyperion ate `-c` as an unknown option of its own. Options are now recognised in any position, and any other dashed argument after the program path belongs to the guest; `--` still ends option parsing.
- **`AT_EXECFN` was filled from `argv[0]`.** For `hyperion run ./prog` that is a relative path, and glibc's `_dl_get_origin()` aborts the process when `AT_EXECFN` and `readlink("/proc/self/exe")` disagree — a statically linked glibc guest died before its `main` ran. Found by the new guest tests, not by inspection.
- **`rax` was left holding the syscall number in a forked child.** `fork()` therefore returned `56` (the `clone` number) instead of `0`, so the child took the parent's branch and waited on a pid that was not its child. The kernel sets `rax = 0` in the child; the handler now does too.
- **`vfork` did not release its parent on successful guest `execve`.** The synchronization pipe used `O_CLOEXEC`, but guest exec is emulated without a host `exec`, so the pipe stayed open until the new image exited. The handler now tracks the internal sync fd and closes it when the guest image is successfully replaced; a regression re-execs `/proc/self/exe` and makes the new image wait for the parent before exiting.
- **A forked child lost the guest image path.** Its fresh syscall handler could not resolve `/proc/self/exe`, which made BusyBox-style self-reexecution fail. The child now inherits its guest argv/image metadata, and its guest pid is initialized from its host pid.
- **The process-creating `clone` subset ignored `child_stack`.** The child now resumes with the requested stack pointer; a raw-clone fixture verifies that execution really enters on that stack. TID pointer preflight also writes back the original guest bytes rather than corrupting them with a zero probe if host fork fails.
- **A JIT-executing parent could crash after `fork`.** On an AArch64 host the parent returned to translated code that had been produced before the process split, and faulted intermittently — roughly one run in ten for a forking guest, with a different test failing each time and no diagnostic output, because the fault also discarded the guest's buffered stdout. The parent now bumps `code_epoch()` on the fork path, so the translation cache drops its blocks and re-translates before the parent resumes. Verified by 300 consecutive AArch64 runs of a forking guest under the JIT (0 failures, previously ~10%) and 15 consecutive runs of the whole guest suite at tier 1 (0 failures, previously 2 in 20). Native x86-64 was unaffected (0 in 200 before and after), which is why this had not surfaced in the host test runs.
- **`install.sh` cloned the wrong repository.** `REPO_SLUG` defaulted to `ransombiyato/hyperion-dbt` (v1) rather than `hyperion-dbt-v2`, so the one-line install command built a different project than the one the script ships in. `CMakeLists.txt`'s `HOMEPAGE_URL` had the same stale slug.
- **`install.sh` now builds the checkout it was run from** when there is one, detects Termux/Android and reports it, installs `hypy` (removable with `--no-hypy`), removes `hypy` on `--uninstall`, and was re-verified for idempotent re-runs, prefixes containing spaces, a non-zero exit with nothing installed when the build fails, and leaving user data in place on uninstall.
- **`scripts/hyperion status` overstated process support:** it listed "single-threaded execution" as the only limitation while `fork`/`execve`/`wait4` were entirely absent, so no program that spawned a subprocess could run. It now states both the supported process semantics and the `ENOSYS` refusals.

### Changed
- **AArch64 cross validation now covers process guests at both tiers.** The cross driver runs the static x86-64 process fixtures through the cross-built AArch64 binary under QEMU at Tier 0 and Tier 1 (and optionally runs BusyBox via `HYPERION_BUSYBOX`). It sets `CMAKE_BUILD_WITH_INSTALL_RPATH=ON`, avoiding CMake's Ninja RPATH regeneration failure.
- **The offline AArch64 toolchain restore now includes host binutils runtime libraries.** `libbfd`/`libopcodes` are cached as a small separate archive, so a fresh `restore` no longer leaves the cross assembler failing to load `libopcodes-2.44-arm64.so`.
- **Documentation reconciled with verified behaviour** across `STATUS.md`, `COMPATIBILITY.md` (new §5.05 process-creation matrix, including the deliberate divergences), `ARCHITECTURE.md`, `ROADMAP.md`, `TESTING.md` and `docs/known-issues.md`. Claims of completeness were replaced with what the tests actually show, and host-run results are labelled separately from AArch64-under-qemu results.

---

## [Unreleased] — Full-System Baseline x86-64 + 128-bit SSE2 ISA, ARMv8 + NEON Tier 1 JIT, and Real Static `glibc` Binary Support (`hyperion-dbt-v2`)

### Added
- **Full baseline x86-64 + 128-bit SSE2 ISA (`decoder.{hpp,cpp}`, `guest_state.hpp`, `interpreter.{hpp,cpp}`):** `FS:` TLS addressing, `DF` + `REP`/`REPZ`/`REPNZ` string ops (`MOVS*`/`STOS*`/`CMPS*`/`SCAS*`), 8/16/32/64-bit GPRs incl. `AH`–`DH`, shifts/rotates, 1/2/3-operand & widening `MUL`/`IMUL`, 8–64-bit `DIV`/`IDIV`, sign-extensions, `CMOVcc`, `SETcc`, `BSWAP`, `BSF`/`BSR`/`TZCNT`/`LZCNT`, `BT`/`BTS`/`BTR`/`BTC`, `XCHG`/`CMPXCHG`/`XADD`, `LEAVE`, `CPUID`/`RDTSC`, and 40+ 128-bit SSE2 packed-integer and scalar floating-point instructions (`27,835 / 27,835` unique `glibc 2.41` instructions decoded).
- **512-entry software MMU `SoftTlb` + 4096-entry decoded ICache:** direct-mapped page addend cache (`56–61 MIPS` native Tier 0 throughput) with executable-only `code_epoch()` invalidation and unconditional syscall data-TLB flush across `brk`/`mmap`/`munmap`/`mprotect`.
- **Full-system AArch64 + 128-bit ARMv8 NEON Tier 1 JIT (`aarch64_encoder.{hpp,cpp}`, `translator.{hpp,cpp}`, `translation_cache.{hpp,cpp}`):** shared trampoline ABI (`enter_jit`, 4096-slot in-JIT direct jump table, shared budget/exit epilogues), inline 8-instruction `SoftTlb` load/store fast path with out-of-line cold stubs and exact instruction-level fault exits, native `PUSH`/`POP`/`CALL`/`RET`/`LEAVE`, branchless `CSEL` conditional exits, self-loop superblocks, and hardware 128-bit ARMv8 NEON SIMD lowering (**104.5×** speedup on `hyperion_tier_bench`; **5.17×–20.12×** end-to-end speedup across 5 real `gcc 14.2 -O2 -static` `glibc 2.41` C workloads totaling `120,313,691` dynamic instructions with `0` step discrepancy).
- **Real-world benchmark & evaluation suite (`benchmarks/workloads/*.c`, `scripts/run_real_benchmarks.sh`, `docs/real-world-evaluation.md`, static ELF CTest smoke gate).**

---

## [0.2.0-pre] — Audits, ISA/ELF64 foundation, initial Tier 0/1 optimizations, and 15 additional Phase 6 syscalls

### Fixed
- **Critical codegen bug (`adds_imm`/`subs_imm` `shift12`):** `aarch64_encoder.cpp` hardcoded `sh = 0` (`bit 22`) on flag-setting shifted immediates and `translator.cpp` dropped `shift12`, miscompiling e.g. `add rax, 0x1000` into `add rax, 1` when `AF` was dead. Verified bit-for-bit against GNU binutils 2.44 and guarded by `translator.shifted_imm12_folded_alu_matches_interpreter`.
- **Critical decoder/interpreter bugs:** fixed `pop rsp` stack adjustment order (`rsp` set to popped value rather than old `rsp + 8`), `SIB mod=00, base=5` (`[disp32]` / `[index*scale + disp32]`), `0x90` with `REX.B == 1` (`xchg r8, rax`), multiple REX prefixes + 15-byte `DecodeError::kTooLong` architectural length limit, and single-instruction `syscall`/`hlt` translation.
- **Address space & NX hazards:** enforced `MemPerm::kExec` on instruction fetch (`Memory::fetch`), preserved existing heap data across page-crossing `set_brk()` calls, honoured `max_map_bytes_`, split mapped regions at page boundaries for sub-region `mprotect`/`munmap`, and added `Memory::code_epoch()` cache invalidation in Tier 0 and Tier 1.
- **Unbuffered POSIX file descriptors:** replaced buffered `std::FILE*` fd slots with refcounted `OpenFileDescription` descriptors and fixed zero-extended 32-bit `mov edi, AT_FDCWD` handling across all `*at` syscalls.
- **Syscall inventory CI enforcement:** upgraded `tools/gen_syscall_inventory.py --check` to cross-check `src/core/syscall.cpp` (`MinimalSyscallHandler::handle` and `is_minimal_syscall`), `docs/syscalls/inventory.tsv`, and generated Markdown tables bidirectionally.

### Added
- **Static ELF64 loader & SysV AMD64 stack (`elf_loader.{hpp,cpp}`):** loads static `ET_EXEC` and static-PIE `ET_DYN` x86-64 binaries with page-aligned W^X `PT_LOAD` mappings, BSS zero-fill, and 16-byte aligned `argc`/`argv`/`envp`/`auxv` stacks; wired into `hyperion info <exe>` and `hyperion run <exe>`.
- **Expanded x86-64 ISA subset:** `[rip + disp32]` addressing, `LEA` (`0x8D`, Tier 0 & Tier 1), `TEST` (`0x85`, `0xA9`, `0xF7 /0`, Tier 0 & Tier 1), near `Jcc rel32` (`0x0F 0x80..0x8F`, Tier 0 & Tier 1), and accumulator-immediate ALU forms (`0x05..0x3D`).
- **Tier 1 & Tier 0 performance engine:** multi-page W^X `ExecArena`, 2-way inline block chaining, hardware `NZCV` condition fusion (`ADD/SUB/CMP/INC/DEC/AND/OR/XOR/TEST -> Jcc`), native self-loop superblocks (**11.19×** narrow / **11.16×** wide Tier 1 speedup), `strb wzr` flag clearing (`OPT-001`), 5-instruction `mvn`+`ubfx` parity (`OPT-002`), MRU region cache, and Tier 0 2048-slot epoch-keyed decoded-instruction cache (**3.47×** narrow / **2.90×** wide speedup).
- **15 additional Linux syscalls (42 total across 11 slices):** `pread64`(17), `pwrite64`(18), `readv`(19), `writev`(20), `dup`(32), `dup2`(33), `uname`(63, always advertising `"x86_64"`), `fcntl`(72), `gettimeofday`(96), `arch_prctl`(158), `set_tid_address`(218), `clock_gettime`(228), `set_robust_list`(273), `dup3`(292), and `prlimit64`(302).

---

## [Unreleased] — Phase 6, tenth syscall slice: `statx`(332)

### Added
- `statx(332)` in both tiers: the modern, extensible stat (950 calls in the
  trace sample). `struct statx` is built only from fixed-width `__u32`/`__u64`
  fields, so its layout is identical on x86-64 and AArch64 and the host's bytes
  are copied into the guest verbatim (D-031); the handler issues `SYS_statx`
  into a raw 256-byte buffer, so it depends on no host struct declaration.
  `AT_FDCWD` only; `AT_SYMLINK_NOFOLLOW` the sole honoured flag, anything else
  reported unimplemented.
- Tests `statx_reports_a_real_file_in_the_guest_layout`,
  `statx_missing_path_is_enoent`,
  `statx_bad_buffer_is_efault_and_bad_pointer_is_efault`,
  `statx_with_a_real_dirfd_is_unimplemented`,
  `statx_unhandled_flag_is_unimplemented`, `statx_is_advertised_as_minimal`.
- `docs/syscalls/inventory.tsv`: `statx` moved from `not-started` to
  `implemented`; regenerated `docs/syscalls/U4.md`.

### Changed
- `DECISIONS.md` D-031 states the rule for forwarding a UAPI structure verbatim:
  rebuild field by field only when the host and guest layouts differ (D-027);
  when the UAPI fixes the widths, forward the bytes.
- The syscall layer now implements 27 numbers across ten slices.

---

## [Unreleased] — Phase 6, ninth syscall slice: `getdents64`(217)

### Added
- `getdents64(217)` in both tiers: directory listing, the highest-frequency
  implementable syscall in the trace sample (3941 calls). The `struct
  linux_dirent64` record is a fixed UAPI layout identical on x86-64 and AArch64,
  so host entries are forwarded verbatim rather than rebuilt field by field
  (D-030); the kernel's own `-EBADF`/`-ENOTDIR`/`-EINVAL` come through unchanged,
  and the guest `count` is clamped to a bounded read.
- `openat` now honours `O_DIRECTORY` (guest `0x10000`, translated to the host
  AArch64 `0x4000`), `O_NONBLOCK` and `O_CLOEXEC` — glibc's `opendir` flag set,
  without which a directory could never be opened.
- Tests `getdents64_lists_a_directory_through_openat`,
  `getdents64_on_a_regular_file_is_enotdir`,
  `getdents64_bad_fd_is_ebadf_and_small_count_is_einval`,
  `getdents64_bad_buffer_is_efault`, `getdents64_is_advertised_as_minimal`,
  `openat_accepts_the_directory_flags`.
- `docs/syscalls/inventory.tsv`: `getdents64` moved from `not-started` to
  `implemented`; regenerated `docs/syscalls/U4.md`.

### Changed
- `DECISIONS.md` D-030 records why `linux_dirent64` is forwarded verbatim
  (shared layout) while `struct stat` is rebuilt field by field (D-027).
- The syscall layer now implements 26 numbers across nine slices.

---

## [Unreleased] — Phase 6, eighth syscall slice: `readlinkat`(267)

### Added
- `readlinkat(267)`, glibc's modern `readlink` entry point, in both tiers.
  It shares the `readlink` body (target copied without a NUL terminator,
  truncation is not an error, `bufsiz == 0` is `-EINVAL`) and adds the leading
  `dirfd`: only `AT_FDCWD` is accepted, matching `openat`/`newfstatat`; any other
  descriptor returns `-ENOSYS` and is counted as unimplemented.
- Tests `readlinkat_with_at_fdcwd_matches_readlink`,
  `readlinkat_with_a_real_dirfd_is_unimplemented`,
  `readlinkat_rejects_zero_size_and_bad_pointer`, `readlinkat_is_advertised_as_minimal`.
- `docs/syscalls/inventory.tsv`: `readlinkat` moved from `not-started` to
  `implemented`; regenerated `docs/syscalls/U3.md`.

### Changed
- The syscall layer now implements 25 numbers across eight slices.

---

## [Unreleased] — README/ROADMAP still described the pre-Phase-4 CLI

### Fixed
- `README.md`'s install section still told users `hyperion run` "does not work
  yet, on purpose" and "prints NOT IMPLEMENTED and exits 2". It has run blobs
  through both tiers since Phase 3/4. Rewritten to describe what actually
  happens, including the x86-64-host interpreter fallback.
- `ROADMAP.md`'s overall-status line said "Phase 0 complete" while its own table
  marked Phase 1 complete.

---

## [Unreleased] — decoder misreported a truncated SIB byte

### Fixed
- `decode_rm_operand` pre-checked the SIB byte with a bare `Reader::has()`,
  which does not set the truncation flag. A ModRM byte whose SIB byte was
  missing (`48 8B 04`) therefore surfaced as `kUnsupportedModrm` instead of
  `kTruncated`. The distinction is load-bearing: a truncated instruction may
  become decodable when a larger window is fetched, an unsupported form never
  will. The byte is now read through the bounds-checked reader.
- Added decoder tests `rejects_truncated_sib` and
  `rejects_truncated_displacement`.

---

## [Unreleased] — the `hyperion` command reported a stale status and refused `run`

### Fixed
- `scripts/hyperion` (the command installed by `install.sh`) hard-coded a Phase 1
  status: it told users the x86-64 decoder, code generation and "the translator
  itself" were NOT IMPLEMENTED, and its `run` verb printed `NOT IMPLEMENTED`
  without ever calling the binary — even though `hyperion run <blob>` has worked
  since Phase 3. `status` now reflects the real phase state and `run` delegates
  to the built binary.
- `install.sh`'s header comment and `ROADMAP.md`'s overall-status line repeated
  the same stale claims.
- Added `tests/cli/wrapper_test.sh` (CTest `cli.wrapper`) so the wrapper cannot
  silently drift from the CLI again: it fails if `status` denies implemented
  features or if `run` does not execute a blob.

---

## [Unreleased] — documentation truth audit

### Fixed — stale docs that contradicted the implemented code
- `docs/known-issues.md` LIM-002/010/011/015 claimed features were missing that
  are implemented (the syscall list stopped at ten numbers and said `openat` was
  unimplemented; LIM-010 said there is no guest stack or memory operands; LIM-011
  said there is no register allocation). Corrected to describe the real state and
  added LIM-016 (the address-space bound is per-mapping, not total).
- `README.md`, `ARCHITECTURE.md` and `ROADMAP.md` still said "four syscall
  slices" / "`open`/`openat` are deliberately not implemented". Corrected to seven
  slices / `AT_FDCWD`-limited `openat`.
- `STATUS.md` §"current state" repeated the three-slice description and the
  `open`/`openat`-not-implemented claim. Corrected.
- `include/hyperion/translation_cache.hpp` claimed there is "no register
  allocation"; per-block register allocation has been present since D-021.

---

## [Unreleased] — guest address space resource bound

### Fixed — guest-controlled `mmap` length could terminate the host process
- `GuestAddressSpace` backed each mapping with host memory sized by the guest's
  `mmap` `length`. An enormous request (`mmap(0, 1<<60, ...)`) reached
  `std::vector::assign` with no handler on the path, so it either threw
  `std::bad_alloc` (terminating the host process) or committed gigabytes —
  both host failures driven by guest input (P0).
- Added a per-mapping cap (`kDefaultMaxMapBytes`, 1 GiB, constructor-tunable)
  checked before any allocation; an oversized request now returns `-ENOMEM`.
  Every region backing allocation (image, anonymous, `brk` growth) is wrapped so
  a genuine allocation failure also becomes a clean `-ENOMEM` instead of an
  uncaught exception. Two new tests (`address_space.an_oversized_*`,
  `syscall.mmap_oversized_request_is_enomem_not_a_host_termination`).
  Documented in SECURITY.md R5 and DECISIONS.md D-029.

---

## [Unreleased] — interpreter fetch-truncation fix

### Fixed — truncated instruction near a region end was silently mis-decoded
- `interpreter.cpp` `step()` always passed a 15-byte window to `decode()`. When
  fewer than 15 bytes were readable (rip near the end of a mapped region) it
  zero-padded the window but still told the decoder the size was 15, so a
  genuinely truncated instruction could be decoded from zero padding as a valid
  instruction and executed with wrong semantics — a silent mis-execution
  (P1). `step()` now tracks the number of bytes actually read and passes that
  true count to `decode()`, so a truncated instruction is reported as
  `kUnsupported`/`kTruncated` and rip is not advanced. A complete instruction
  that merely sits at the end of a region still executes.
- The Tier 1 translation cache already passed the true available count, so this
  was an interpreter-only defect; the two tiers now agree.
- Corrected the misleading RET comment in `step()` (it claimed RET pops the
  return address; it does not) and pinned the real "top-level RET halts, stack
  untouched" behaviour with a test. Four new regression tests in
  `tests/unit/test_interpreter.cpp`.

---

## [Unreleased] — Phase 6 (access + readlink)

### Added — `access`(21) and `readlink`(89) (seventh syscall slice)
- `access`(21): forwards to the host with `mode` restricted to `F_OK`/`R_OK`/
  `W_OK`/`X_OK` (any other bit is `-EINVAL`, as the kernel does). Reports the
  host process's permissions, consistent with the `openat` policy (D-025).
- `readlink`(89): copies a symlink target into the guest buffer. As readlink(2):
  the result is not NUL-terminated, a target longer than `bufsiz` is truncated
  (not an error), and `bufsiz == 0` is `-EINVAL`. Both use the shared bounded
  `read_guest_cstr`, and a huge guest `bufsiz` is clamped rather than allocated.
- Five new unit tests (`syscall.access_*`, `syscall.readlink_*`). Inventory:
  `access` and `readlink` moved from `not-started` to `implemented` (24 of 375).
  Docs regenerated (0 errors/warnings).

---

## [Unreleased] — Phase 6 (newfstatat)

### Added — `newfstatat`(262) (sixth syscall slice)
- `newfstatat`(262): the path-based metadata call glibc's `stat()`, `lstat()` and
  `fstatat()` all funnel through. Limited to `AT_FDCWD` (a real directory
  descriptor returns `-ENOSYS`). `AT_SYMLINK_NOFOLLOW` selects `lstat`
  semantics; any other flag (e.g. `AT_EMPTY_PATH`) is rejected as unimplemented
  rather than silently dropped. The pathname is read from guest memory with the
  same bounded `read_guest_cstr` as `openat`, and a missing path is `-ENOENT`.
- Refactored the guest (x86-64) `struct stat` writer out of `do_fstat` into a
  shared `write_guest_stat`, so `fstat` and `newfstatat` write the ABI-sensitive
  layout in exactly one place — still field-by-field, never `memcpy`'d, because
  the host layout differs on AArch64.
- Seven new unit tests (`syscall.newfstatat_*`), including a dangling-symlink
  case that proves `stat` follows (ENOENT) while `AT_SYMLINK_NOFOLLOW` reports
  the link itself (`S_IFLNK`). Inventory: `newfstatat` moved from `not-started`
  to `implemented` (22 of 375). Docs regenerated (0 errors/warnings).

---

## [Unreleased] — Phase 6 (identity getters + getcwd + getrandom)

### Added — process-identity getters, `getcwd`(79), `getrandom`(318) (fifth syscall slice)
- `getpid`(39), `gettid`(186), `getuid`(102), `geteuid`(107), `getgid`(104),
  `getegid`(108), `getppid`(110): forward to the host process. Hyperion runs the
  guest in-process, so the guest sees the host's ids; `gettid` reports the host
  thread id and assumes single-threaded execution (revisited when `clone`
  lands).
- `getcwd`(79): copies the host working directory into a bounded guest buffer.
  `-ERANGE` when the path plus its NUL does not fit, `-EFAULT` on a pointer the
  memory model rejects, `-EINVAL` for size 0 — matching getcwd(2).
- `getrandom`(318): fills a bounded guest buffer from the host in 4 KiB chunks.
  Only the two defined flag bits (`GRND_NONBLOCK`, `GRND_RANDOM`) are accepted;
  any other bit is `-EINVAL`, as the kernel does. A `count` of 0 returns 0
  without touching the buffer; an unmapped destination is `-EFAULT`.
- Five new unit tests (`syscall.identity_getters_*`, `syscall.getcwd_*`,
  `syscall.getrandom_*`). Two existing tests that used `getpid`(39) as their
  example of an *unimplemented* number were re-pointed at `futex`(202), since
  39 is now implemented. Inventory: the nine numbers moved from `not-started` to
  `implemented`; the implemented set is now 21 of 375. Docs regenerated
  (`python3 tools/gen_syscall_inventory.py`, 0 errors/warnings).
- Added `kErange`(34) and `kEintr`(4) to the errno constants.

---

## [Unreleased] — Phase 6 (openat + fstat)

### Added — `openat`(257) and `fstat`(5) (fourth syscall slice)
- `openat`(257): opens a host file on the guest's behalf and allocates the
  lowest free guest descriptor ≥ 3 (as the kernel does). Limited to `AT_FDCWD`
  and a bounded guest pathname (read with `read_guest_cstr`, `-EFAULT` on a bad
  pointer, `-ENAMETOOLONG` past `PATH_MAX`). An `O_*` flag outside the honoured
  set (`O_RDONLY`/`O_WRONLY`/`O_RDWR`/`O_CREAT`/`O_TRUNC`/`O_APPEND`/`O_EXCL`),
  or a real `dirfd`, returns `-ENOSYS` and is reported *unimplemented* rather
  than resolved against the wrong base or given weaker semantics. The opened
  stream is handler-owned and released by `close`; a stream attached with
  `adopt()` remains borrowed. This is not a sandbox: `openat` forwards to the
  host kernel (SECURITY.md).
- `fstat`(5): reports the **guest** (x86-64) `struct stat` layout, built
  field-by-field from the host's rather than `memcpy`'d, because the host layout
  differs on AArch64. `-EBADF` for a bad descriptor, `-EFAULT` for an
  out-of-range stat buffer.
- `MinimalSyscallHandler`'s descriptor table is now
  `std::vector<GuestDescriptor>` (`stream` + `owned`), so `close` closes a
  stream the handler opened but leaves an adopted one to its owner.
- Ten new unit tests (`syscall.openat_*`, `syscall.fstat_*`), including the
  ABI-sensitive check that `st_size` lands at the x86-64 offset 48 and `st_mode`
  carries `S_IFREG`. Inventory: `openat` and `fstat` moved from `not-started` to
  `implemented`; docs/syscalls regenerated.

## [Unreleased] — Phase 6 (guest descriptor table + read/close/lseek)

### Fixed — clang build (CI `build & test (clang)` and `ASan/UBSan`)
- `src/core/syscall.cpp` indexed the descriptor table with a signed `int64_t`
  (`files[fd]`), which clang's `-Wsign-conversion` rejects under
  `HYPERION_WARNINGS_AS_ERRORS=ON`. gcc's `-Wconversion` did not flag it, so
  the local build passed while CI failed. The bounds check and index conversion
  now live in a single `lookup_stream(files, fd)` helper, so no signed value
  reaches `vector::operator[]`. `scripts/dev.sh clang-build` added so this
  class of failure is catchable locally before pushing.

### Fixed — gcc now catches the same sign-conversion bug as clang
- `CMakeLists.txt` adds **`-Wsign-conversion`** to the warning set. clang
  enables it implicitly under `-Wconversion`; gcc does not, so a gcc-only host
  silently accepted `files[fd]` and `refs[i] += (r[i] ? 1 : 0) + ...`. Both
  are now rejected by `scripts/dev.sh build` on gcc, so reproducing this class
  of CI failure no longer requires a local clang.
- `src/core/translator.cpp`: cast the flag-reference increment to
  `std::size_t` (second latent instance of the same bug, found only after
  enabling the flag).

### Added — guest descriptor table (third syscall slice)
- `MinimalSyscallHandler` now owns a **guest descriptor table**
  (guest fd → host `FILE*`), so `read`/`close`/`lseek` operate on descriptors
  the handler controls rather than on raw host fds. The table starts with
  fd 0/1/2 on the process's stdin/stdout/stderr; `adopt(guest_fd, stream)`
  attaches an already-open host stream under another guest fd. A guest fd that
  is not in the table is `-EBADF`, so a guest can never name an arbitrary host
  descriptor. The handler never `fclose`s a borrowed stream.
- `read`(0): copies from an adopted stream into guest memory in 4 KiB chunks,
  returning a short count at end-of-stream (as `read(2)` does), `-EBADF` for a
  bad descriptor and `-EFAULT` for an out-of-range guest buffer.
- `close`(3): drops the guest descriptor (does not close the borrowed stream).
- `lseek`(8): `SEEK_SET`/`SEEK_CUR`/`SEEK_END` over an adopted stream;
  `-ESPIPE` for a non-seekable stream, `-EINVAL` for a bad `whence`.
- `docs/syscalls/inventory.tsv` moves `read`(0), `close`(3) and `lseek`(8) from
  `not-started` to `implemented`; `docs/syscalls/` regenerated.
- New errno constants `kEperm`, `kEnoent`, `kEio`, `kEspipe`.
- Tests: `syscall.read_copies_from_adopted_stream_into_guest_memory`,
  `syscall.read_of_bad_fd_is_ebadf`, `syscall.read_out_of_range_buffer_is_efault`,
  `syscall.read_short_read_at_end_of_stream`,
  `syscall.close_drops_the_descriptor_but_not_the_stream`,
  `syscall.close_of_bad_fd_is_ebadf`, `syscall.lseek_repositions_an_adopted_stream`,
  `syscall.lseek_end_and_cur_whence_work`,
  `syscall.lseek_bad_whence_is_einval_and_bad_fd_is_ebadf`,
  `syscall.adopt_refuses_an_occupied_descriptor`,
  `syscall.open_is_still_unimplemented`.

### Not added (stated, not implied)
- `open`/`openat` remain **unimplemented**: a guest can read inherited
  descriptors but cannot open host files. The descriptor table maps to `FILE*`,
  so `O_NONBLOCK`, `O_APPEND`, `pread`/`pwrite`, `fcntl` and `dup` are also not
  expressible yet.

---

## [Unreleased] — Phase 6 (guest address space + memory syscalls)

### Added — guest address space (DECISIONS.md D-023)
- `GuestAddressSpace` (`include/hyperion/address_space.hpp`,
  `src/core/address_space.cpp`): guest virtual memory as a set of regions, each
  with a base, length and `MemPerm` (read/write/exec), with page-granular
  permission checks on every access. `map_image`, `map_anonymous`, `read`,
  `write`, `protect`, `unmap`, `find_region`, `range_is_free`, and a reserved
  `[heap_base, heap_end)` window so the heap and `mmap` allocations cannot
  overlap. `set_brk` maps the pages a grow touches.
- Unit tests in `tests/unit/test_address_space.cpp`: map/read/write, permission
  denial on a read-only region, `brk` growth, and page-granular range checks.

### Added — memory syscalls (inventory status → `implemented`)
- `mmap`(9) (anonymous private mappings only), `mprotect`(10), `munmap`(11) and
  `brk`(12) are implemented in `MinimalSyscallHandler` over a caller-supplied
  `GuestAddressSpace`. `docs/syscalls/inventory.tsv` moves all four from
  `not-started` to `implemented`; `docs/syscalls/` regenerated.
- A memory syscall's return value and its *implemented* status are tracked
  separately (`MemResult`): a file-backed `mmap` returns `-ENOSYS` **and** is
  counted in `RunResult::unimplemented_syscalls`, so a partial implementation is
  never reported as support. Without an address space the four return `-ENOSYS`
  and count as unimplemented, rather than pretending success.
- Tests: `syscall.mmap_anonymous_returns_a_mapping_that_is_usable`,
  `syscall.mmap_zero_length_is_einval`,
  `syscall.mmap_file_backed_is_enosys_not_a_wrong_mapping`,
  `syscall.mprotect_and_munmap_round_trip`, `syscall.brk_returns_the_new_break`,
  `syscall.memory_syscalls_without_an_address_space_are_enosys`,
  `syscall.is_minimal_syscall_reports_the_new_set`.
- End-to-end integration: `syscall.mmap_end_to_end_under_the_interpreter` runs a
  real guest program (mmap → store through the returned pointer) under Tier 0;
  `syscall.mmap_end_to_end_under_generated_code_stops_at_memory_operand` pins
  the honest Tier 1 boundary — the mmap syscall is handled, then the block stops
  at the memory-operand store because the code generator is register-only.

### Fixed
- `mmap`'s `fd`/`offset` are no longer inspected for `MAP_ANONYMOUS`: the kernel
  ignores them, so the handler does too, instead of returning `-ENOSYS` for a
  legal anonymous mapping that happened to pass a real fd.
- `set_brk` now actually maps the heap memory it grows into (a design gap in the
  first address-space draft).

---

## [Unreleased] — Phase 5 (performance optimizations 3–6)

### Added — flag liveness (BENCHMARKS.md §4.2.3)
- `translate_block` is now three passes: decode the block, run backward flag
  liveness over it, then emit. Each ALU instruction computes only the flags read
  before being overwritten; `live_out` starts at all flags at the block exit
  (flags persist across blocks, so there is no cross-block analysis).
- INC/DEC no longer emit an explicit CF save/restore: `flags_written()` excludes
  CF for them, so CF is preserved structurally.
- Generated code for the fixed workload fell 584 → 352 bytes (−39.7%); the
  size-guard program fell 148 → 90 words. No wall-clock claim under `qemu-user`.
- New deterministic guard: `translator.dead_flags_are_not_computed`.

### Added — direct-mapped dispatch (BENCHMARKS.md §4.2.4)
- `TranslationCache` consults a 4096-entry direct-mapped table (low 12 bits of
  the guest PC) before the hash map. The map stays authoritative, so a collision
  is a safe fall-through, never a wrong block.
- Measured dispatch hit rate on the fixed workload: 199,998 hits / 3 misses
  (99.9985%); counters exposed in the benchmark JSON.
- New test: `translation_cache.direct_map_collision_falls_back_to_map`.

### Added — ALU immediate folding (BENCHMARKS.md §4.2.5)
- `add/sub/cmp reg, imm` with an immediate that fits the AArch64 12-bit ADD/SUB
  immediate form folds into one ADDS/SUBS-immediate, instead of materialising the
  constant first. Applied only when AF is dead (AF needs `b` in a register).
- Size-guard program fell 90 → 88 words.
- New test: `translator.alu_immediate_is_folded`.

### Added — guest-register allocation within a block (BENCHMARKS.md §4.2.6)
- Pass 1.5 ranks guest registers by use count in the block and maps the hottest to
  callee-saved host registers (x19..x28), saved/restored around the block with
  `stp`/`ldp`. `mov`, the ALU ops and `inc`/`dec` operate directly on the
  allocated register when both operands are allocated.
- A **profitability guard** declines allocation when the prologue/epilogue would
  cost more than the saved loads. The first version allocated every touched
  register and *regressed* the tiny benchmark loop (364 → 388 bytes); the guard
  fixes that (356 bytes on, 364 off).
- New `--wide` workload (12-instruction body, 11 guest registers) added to
  `hyperion_tier_bench` because the narrow loop cannot show the win. Result:
  620 → 608 bytes and 10.70 → 9.05 ns/guest-instruction (RA off → on),
  reproducible across runs.
- New tests: `translator.differential_register_allocation_many_registers`,
  `translator.differential_register_allocation_immediates`.

### Fixed — halted `rip` was the block start (BENCHMARKS.md §4.2.7)
- A block ending in `HLT` (or `RET`) returned `kHalted` without writing `rip`, so
  the reported halted `rip` was the block's first address instead of the `HLT`
  address. Fixed by emitting `rip = insn.address` on the halt path. Found by the
  new many-register differential test (the first program to end a
  multi-instruction block in `HLT` and compare full state).

### Added — repo-local tool cache (`scripts/toolcache.sh`)
- The container wipes `/usr` between sessions but the repository directory
  persists, so the AArch64 emulator and cross toolchain are cached under
  `tools/cache/` (git-ignored). `scripts/bootstrap.sh` populates the cache;
  `scripts/toolcache.sh restore` reinstates the toolchain offline after a reset.
  The benchmark and cross-test scripts pick up the cached `qemu-aarch64-static`
  automatically. Recovery path verified by deleting the sysroot and restoring it.

### Notes
- All four optimizations were accepted on **deterministic** evidence (generated
  code size, dispatch counters) plus a relative Tier 1 time signal; `qemu-user`
  wall-clock is too noisy for absolute claims, so no hardware speedup is claimed.
  The interpreter remains the correctness oracle and the differential suite must
  pass unchanged.

---

## [Unreleased] — Phase 6 (first syscall slice)

### Added — guest syscall layer (first slice)
- `include/hyperion/syscall.hpp`, `src/core/syscall.cpp`: the `SyscallHandler`
  interface (guest state in, result written to `rax`) and
  `MinimalSyscallHandler`, which implements `write`(1), `exit`(60) and
  `exit_group`(231) and returns `-ENOSYS` for every other number.
- The x86-64 `syscall` instruction (`0F 05`) is decoded as `Opcode::kSyscall`.
- Both execution tiers route syscalls through the handler: the interpreter
  handles it inline; the translator ends the block with
  `BlockResult::kSyscall` and the dispatcher runs the handler, so a syscall is
  never skipped or executed with wrong guest state.
- `RunResult::unimplemented_syscalls` records how many `-ENOSYS` calls the
  program made, so an unimplemented call is visible, never silent.
- Error convention matches Linux: failures return a negative errno in `rax`
  (e.g. `-EBADF`, `-EFAULT`); a guest buffer outside the mapped region yields
  `-EFAULT` rather than a host crash.
- 11 new tests (`tests/unit/test_syscall.cpp`): decoding, handler semantics
  (good/bad fd, out-of-range pointer, exit code, unknown number), interpreter
  integration, and a Tier 1 differential that runs generated AArch64 under
  `qemu-user`.

### Changed — syscall inventory
- `docs/syscalls/inventory.tsv`: `write`, `exit` and `exit_group` moved from
  `not-started` to `implemented`; the seven generated files regenerated. Every
  other entry remains `not-started`.

### Notes
- Exactly three syscalls are implemented. This is a first slice, not general
  Linux compatibility; see `docs/known-issues.md` LIM-002.

---

## [Unreleased] — Phase 6 (syscall inventory)

### Added — evidence-based syscall inventory
- `docs/syscalls/inventory.tsv`: the canonical, one-row-per-syscall data source
  (number, name, tier, status, purpose, evidence).
- `docs/syscalls/kernel-table-x86_64-6.12.tsv`: the pinned Linux v6.12 x86-64
  syscall table (`arch/x86/entry/syscalls/syscall_64.tbl`), reduced to the 375
  `common`/`64` entries.
- `docs/syscalls/U1.md`–`U4.md`, `E1.md`–`E3.md`: the seven required inventory
  files, generated from `inventory.tsv`. Distribution: U4=39, U3=75, U2=32,
  U1=14, E1=8, E2=100, E3=107 (375 total).
- `docs/syscalls/README.md`: tier criteria, the pinned-kernel policy and the
  evidence policy.
- `tools/gen_syscall_inventory.py`: validates the inventory against the pinned
  table (missing, duplicate, misnumbered, mis-tiered, x32, invalid status) and
  regenerates the seven Markdown files.
- `scripts/trace-syscalls.sh`: the reproducible strace sweep behind the
  `trace:N` evidence values. It is honest about its limits: a small x86-64-host
  sample, treated as a lower bound; unmeasured entries are `provisional`.
- Wired into the suite as `syscall.inventory_valid` and into CI as the
  `inventory` job, which also fails if the generated Markdown is stale.

### Notes
- The inventory records *classification and priority*, not implementation. Every
  entry is `not-started`; no syscall is claimed as working.

---

## [Unreleased] — Phase 5

### Added — performance measurement
- `benchmarks/tier_bench.cpp` (target `hyperion_tier_bench`): runs one fixed
  guest loop through Tier 0 (interpreter) and Tier 1 (translator), reporting
  median ns and ns per retired guest instruction with an IQR spread, using the
  shared methodology (one discarded warm-up, repeated samples, a correctness
  check on the final register). Tier 1 fields are reported **unavailable** on a
  host that cannot execute generated AArch64 code, rather than fabricated.
- `scripts/bench-tier-qemu.sh`: reproducible Tier 0 vs Tier 1 measurement on an
  AArch64 host (under `qemu-user` on a development machine).

### Recorded — first baseline (BENCHMARKS.md §4.2)
- Fixed loop, 600,002 retired guest instructions, median of 7 under
  `qemu-aarch64`: Tier 0 = 491.9 ns/guest-ins, Tier 1 = 38.4 ns/guest-ins,
  ratio 0.078 (Tier 1 ~12.8× faster). Documented explicitly as a *relative*
  signal between Hyperion's own tiers, not a hardware performance claim.

### Changed — Optimization 1: parity (PF) fast path (BENCHMARKS.md §4.2.1)
- `emit_parity` (`src/core/translator.cpp`) rewritten from a per-bit
  accumulation loop (19 emitted AArch64 instructions, including setup) to a
  shifted-`EOR` XOR-fold (6). x86 PF semantics unchanged.
- New encoder primitive `Encoder::eor_shifted` (EOR with a shifted register),
  with encodings verified against `aarch64-linux-gnu-as` for all four shift
  types. A first attempt wrongly set bit 21; the new encoder test caught it.
- Measured by an interleaved A/B against the pre-change commit on the same host:
  generated code for the fixed workload **904 → 644 bytes (−28.8%,
  deterministic)**, Tier 1 wall time **−17.5%**, Tier 1/Tier 0 ratio
  0.0817 → 0.0733. Correctness re-verified by the full differential suite under
  `qemu-aarch64` (99 passed, 0 failed).

### Added — deterministic translation-size diagnostics
- `TranslationCache::total_code_bytes()` and `CachedBlock::code_bytes`, plus a
  `tier1_translator.generated_code_bytes` field in the benchmark JSON. These are
  deterministic, so they act as a regression guard where `qemu-user` timing
  noise (IQR of several percent) would hide a code-size regression.
- Test `translator.translated_code_size_is_bounded` pins the block count and an
  upper bound on generated words for a fixed guest loop.

### Changed — Optimization 2: dead `rip` store elimination (BENCHMARKS.md §4.2.2)
- `emit_one` gained a `write_rip` parameter. Translated blocks emit body
  instructions with `write_rip=false` and write `rip` exactly once, at the
  block exit; nothing inside a block reads `rip`, so the per-instruction
  `movz`+`movk`+`str` was dead. The single-instruction translator keeps
  `write_rip=true` (its caller reads `rip` back).
- Generated code for the fixed workload **644 → 584 bytes (−9.3%)**; Tier 1 wall
  time −4% in an interleaved A/B. Delta fully accounted for (5 body instructions
  × 3 instructions). The differential suite compares the final `rip`, so a
  misplaced write fails hard.
- Tightened `translator.translated_code_size_is_bounded` from a loose `< 200`
  word bound to an upper bound at the current count (148 words), so any future
  growth fails CI while a reduction is still allowed.

### Docs
- Split `STATUS.md` phase sections correctly (two headings both said "Phase 3";
  the earlier block is Phase 2). Corrected the stale claim that no translator
  exists.

---

## [Unreleased] — Phase 4

### Added — basic dynamic binary translator
- `include/hyperion/translation_cache.hpp`, `src/core/translation_cache.cpp`:
  `TranslationCache`, the Tier 1 dispatcher. It looks up the block for
  `state.rip`, translates it on first sight, executes it, and repeats until the
  program halts, an unsupported instruction is reached, or a guest-instruction
  step limit is hit. It **never executes an instruction the translator refused**:
  an unsupported instruction stops the run with `RunStatus::kUnsupported` and
  leaves `rip` on the offending instruction.
- `translate_block` (`src/core/translator.cpp`) lowers a whole basic block:
  it decodes from a guest PC until a terminator (`jmp`/`jcc`/`hlt`), the end of
  the buffer, or an untranslatable instruction. It returns the generated code,
  a `BlockExit` (fall-through / jmp / jcc / halted), a target/fall-through hint,
  and the number of guest instructions covered. The authoritative next PC is
  always the `rip` field the generated block writes.
- `include/hyperion/run.hpp`: `RunStatus`/`RunResult` extracted from
  `interpreter.hpp` so both execution tiers report through the same types and
  can be compared directly.

### Added — `hyperion run`
- `src/cli/main.cpp`: `hyperion run <file> [--tier=0|1] [--max-steps=N] [--dump]`
  executes a raw x86-64 code blob through the reference interpreter (tier 0) or
  the AArch64 translator (tier 1). On a host that cannot execute generated
  AArch64 code, tier 1 falls back to the interpreter and says so on stderr.

### Added — decoder
- `0xEB` (`jmp rel8`): short unconditional jump, the companion to the
  `0x70..0x7F` short `jcc`. Decoder test `decoder.jmp_short_rel8`.

### Added — tests
- `tests/unit/test_translator.cpp`: block-formation scope tests (terminator,
  unsupported cut, buffer-end cut), whole-program differentials against
  `run_program` (counted loop; taken/not-taken branches across flag states), and
  dispatcher tests (stops at unsupported; infinite loop → step limit).
- `tests/unit/test_translation_cache.cpp`: the cache refuses to run when the
  host cannot execute generated code; caches blocks and agrees with the
  interpreter on AArch64.
- `tests/cli/cli_smoke.sh` + CTest entry `cli.smoke`: runs the real CLI through
  both tiers and requires them to agree. The cross pipeline runs it under
  `qemu-user` with `HYPERION_EMULATOR`, requiring tier 1 to have translated a
  non-zero number of blocks.

### Fixed
- **Infinite loop on an unsupported first instruction.** A block whose first
  instruction was unsupported was emitted as a zero-instruction fall-through
  block, so the dispatcher re-entered the same PC forever. `translate_block` now
  reports the error when the block would be empty.
- **Short program images failed to translate.** The dispatcher's code-fetch
  window shrank only to 15 bytes, so a 13-byte blob could not be read. The window
  now shrinks to a single byte.
- **The dispatcher could exceed the step limit.** It checked the limit before
  running a block and then added the whole block's count, so a 1,000,000-step
  run could retire 1,000,001 instructions. A block that does not fit the
  remaining budget is now not started, so the bound is hard.

### Added — more guest programs
- `translator.differential_nested_loops` (two backward branches) and
  `translator.differential_branch_inside_loop` (taken/not-taken exits that
  reconverge and alternate). Both compare against `run_program` and also assert
  an independently computed final value, so agreement cannot be a shared
  mistake.

---

## [Unreleased] — Phase 3

### Added — x86-64 to AArch64 translator (increment 1)
- `include/hyperion/translator.hpp`, `src/core/translator.cpp`:
  `translate_instruction` lowers one decoded x86-64 instruction to a
  self-contained AArch64 block with the signature `BlockResult block(GuestState*)`.
  Supported: `mov`, `add`, `sub`, `and`, `or`, `xor`, `cmp`, `inc`, `dec`,
  `nop`, `hlt` with 32/64-bit register or immediate operands. Guest registers
  live in the `GuestState` struct; generated code loads/stores them.
- All six x86 flags (CF/PF/AF/ZF/SF/OF) are computed **in generated code** from
  AArch64 NZCV plus two ALU identities, never delegated to the interpreter, so
  the differential test is an independent check (D-017).
- Out-of-scope instructions are refused with an explicit `TranslateError`
  (`jmp`/`jcc`/`ret`/`push`/`pop` → `kUnsupportedOpcode`; memory operands →
  `kUnsupportedOperand`) rather than miscompiled.
- `tests/unit/test_translator.cpp`: scope tests run on every host; the
  differential tests execute generated blocks under `qemu-user` on AArch64 and
  compare every register and all six flags against the interpreter, including an
  847-case operand sweep over carry/overflow/parity boundaries. On x86-64 they
  report **skip**, not a false pass.

### Added — AArch64 encoder and executable memory
- `include/hyperion/aarch64_encoder.hpp`, `src/core/aarch64_encoder.cpp`:
  `a64::Encoder`, a checked AArch64 emitter for the operations the Phase 3
  translator needs first — add/sub/and/orr/eor (register and immediate forms),
  cmp/cmp-imm, mov/movz/movk/mov-imm64, b/b.cond/ret/nop, and label-based
  branch fixups. It throws on an out-of-range register or immediate instead of
  emitting a wrong word. **Every encoding is verified against GNU as/objdump**;
  the expected words in `tests/unit/test_aarch64_encoder.cpp` are binutils
  output, not values written from memory.
- `include/hyperion/exec_mem.hpp`, `src/core/exec_mem.cpp`: `ExecMem`, a
  page-aligned region that is written read-write, then made read-execute with an
  instruction-cache clear (`W^X`, per `DECISIONS.md` D-016). Writing after
  `finalize()` is refused rather than silently corrupting code.
- `tests/unit/test_exec_mem.cpp`: on AArch64 (including under `qemu-user` in
  CI) these generate and **execute** real code — `add x0,x0,x1; ret` and a
  `1..n` summation loop — and check the results. On x86-64 they report **skip**,
  not a false pass.

### Fixed
- `include/hyperion/decoder.hpp`: the `kJcc` comment said "rel32"; the decoder
  implements rel8 short forms only. Comment corrected to match the code and
  `COMPATIBILITY.md` §2.

### Decisions
- D-014 (decoder contract is the subset boundary), D-015 (checked encoder),
  D-016 (W^X executable memory), D-017 (flags recomputed in generated code).

---

## [Unreleased] — Phase 2

### Added — guest state, decoder, reference interpreter
- `include/hyperion/guest_state.hpp`, `src/core/guest_state.cpp`: the guest
  register file (16 GPRs, index order matching the x86-64 ModRM/REX encoding),
  `rip`, and a `Flags` struct with `pack_flags`/`unpack_flags` mapping to and
  from architectural RFLAGS bits.
- `include/hyperion/decoder.hpp`, `src/core/decoder.cpp`: an x86-64 decoder for
  a documented subset — MOV/ADD/SUB/AND/OR/XOR/CMP/INC/DEC/PUSH/POP/JMP/Jcc
  (rel8)/NOP/HLT — with REX, ModRM and SIB support for
  `base + index*scale + disp`. Every unsupported byte sequence is **rejected**
  with a specific `DecodeError` (`kUnsupportedPrefix`, `kUnsupportedModrm`,
  `kUnknownOpcode`, `kTruncated`); it never guesses.
- `include/hyperion/interpreter.hpp`, `src/core/interpreter.cpp`: a reference
  interpreter over a `FlatMemory` region, implementing the ALU flag model (CF,
  PF, AF, ZF, SF, OF), effective-address computation, and x86-64's 32-bit
  zero-extension rule. This is the **correctness oracle** for Phase 3.
- 38 new unit test cases: `test_guest_state.cpp` (7), `test_decoder.cpp` (19),
  `test_interpreter.cpp` (16, including four that run whole programs).

### Added — program-image runner and the subset contract
- `run_program(memory, state, max_steps)`: runs a program image until HLT/RET,
  a fault, or a step limit, and returns the final state, retired-instruction
  count and a reason. A convenience layer over `step()` with no new semantics;
  the default step limit exists so a non-halting guest program is *reported*,
  not allowed to hang the suite.
- Tests run a real `dec`/`jnz` loop (sum 1..10 = 55) and cross-check the result
  against native x86-64; a second test proves an infinite loop reports
  `kStepLimit`; a third proves execution stops at the first unsupported
  instruction rather than running past it.
- `COMPATIBILITY.md` §2 rewritten from a *plan* into a *specification*: it now
  documents exactly what the decoder accepts (opcodes, encodings, operand
  sizes, addressing forms, `DecodeError` cases) and lists the planned-but-
  unbuilt instructions in a separate section so the plan is not mistaken for
  working code. §3 and §4 updated to the real flag and register coverage.

### Added — hardware differential test
- `tests/differential/test_differential.cpp`: compares the interpreter against
  **real x86-64 hardware** by executing the same operation natively (inline
  asm, flags captured through `pushfq`) and in the interpreter, across many
  operand values and flag inputs. 8 cases. This is the Phase 2 acceptance test:
  the interpreter is only trusted because an independent implementation agrees.
- Added `HYPERION_SKIP`: the harness can now report a test as **skipped**
  (not executed), separately from passing. On non-x86-64 hosts the differential
  cases SKIP rather than falsely reporting coverage — on AArch64 the suite
  reports `0 passed, 8 skipped`.

### Fixed — two bugs found by the differential test
- `alu::add` never set CF for 64-bit operands: the check `sum > mask` is
  impossible when `mask == ~0`, so the carry test could not fire. Corrected to
  `(sum < ua) || (sum > mask)`.
- `Jcc` short jumps (`0x70..0x7F`) were decoded with a rel32 displacement
  instead of rel8, producing wild branch targets. The rel32 forms are
  `0x0F 0x80..0x8F` and are still unsupported.

---

## [Unreleased] — Phase 1

### Added — one-line installer and `hyperion` command
- `install.sh`: `curl -fsSL .../install.sh | bash` fetches, builds and tests the
  project, then installs a `hyperion` command into `<prefix>/bin` (default
  `~/.local`). No root, nothing written outside the prefix, and it aborts
  rather than install a build that fails its own tests.
- `scripts/hyperion`: a front end with `status`, `doctor`, `version`, `build`,
  `test`, `bench`, `cross-test`, `clean`. `run` and `info` exist but exit 2 with
  `NOT IMPLEMENTED` — there is no translator yet, and an honest refusal is
  better than a command that silently does nothing.

### Added — baseline benchmark methodology
- `include/hyperion/bench_stats.hpp`, `src/core/bench_stats.cpp`: min, p25,
  median, p75, max, mean, population standard deviation and `iqr_fraction` (the
  noise indicator), computed in the core library so the Phase 8 optimisation lab
  reports results the same way.
- The harness now takes `--repeats` (default 9, minimum 2), discards one warm-up
  sample and reports the full spread; JSON schema `hyperion.bench.host.v1` →
  `v2`.
- Measured host calibration recorded in `BENCHMARKS.md` §4.1: median
  **2.0568 ns/op**, 1.2 % spread, checksum stable, with a scaling validation at
  1e6/1e7/1e8 iterations. These are harness figures, not a translator claim.
- 8 unit tests for the statistics; `bench.methodology` and
  `bench.timing_not_eliminated` CTest entries; `bench.rejects_single_sample`
  negative test.

### Fixed — BENCH-001, a fabricated benchmark result
- The Phase 0 harness reported ~0.00003 ns/op (~30 ps per iteration, far below
  one clock cycle) because `-O2` hoisted the loop-invariant calibration call out
  of the repeat loop. The checksum stayed correct, so the failure was silent.
- Found by checking that total time scales with `--iterations`; it did not.
- Fixed with an `asm` optimisation barrier plus a plausibility check: a result
  faster than one clock cycle per iteration is rejected, and the harness exits
  non-zero, rather than publishing it. Regression test added.

### Added — prior-art survey and reuse decision
- `docs/prior-art.md`: FEX-Emu, Box64, QEMU TCG, Zydis, Capstone, LLVM,
  Unicorn, Dynarmic, with licences verified against upstream licence files.
- `DECISIONS.md` D-013: the reuse-vs-build boundary.
- `docs/prior-art.md` §6: the attempted AArch64 UML cross-build. It configures
  but does not compile, and the same failure occurs on stock x86-64 UML, so it
  is a host toolchain problem rather than a port defect. The port is recorded as
  *plausible but unverified*.

### Changed — scope correction (2026-10-19)
- **Withdrawn** the Phase 0 assumption that kernel/system-mode emulation is an
  explicit non-goal. It capped the project's compatibility ceiling below its
  actual goal and was made without measurement. See `DECISIONS.md` D-011.
- Added `docs/architecture-feasibility.md`: an evaluation of user-mode
  translation, system-mode emulation, and hybrid kernel-as-a-process designs,
  with the Android host-kernel constraints that bound all of them.
- Added `DECISIONS.md` D-012 recording the kernel-as-process tier as
  **provisional**, pending validation on the reference device.
- Added a constraint on Phase 6: the syscall layer must be abstracted behind an
  interface so syscalls can be directed to the host kernel or a user-space
  kernel. Nothing about that second path is implemented.

### Added — feasibility evidence
- `experiments/uml/` — a reproducible experiment that boots a **real Linux
  kernel as an unprivileged user-space process** (no root, no KVM) and drives
  it to a `systemd` multi-user boot.
- Measured result: UML kernel `6.12.101` booted from an isolated ext4 image
  (host root verified *not* visible), `systemd 257.13-1~deb13u1` ran as PID 1
  and reached `multi-user.target` and `graphical.target`; `dbus`,
  `systemd-logind` and `getty@tty1` started; all six namespace types worked
  inside the guest.
- Desk research confirming an **AArch64** UML port exists
  (`zalexdev/linux-um-arm64`, `ARCH=um SUBARCH=arm64`), verified to contain real
  architecture code and Android/bionic support. **Not** built or run by this
  project, and **not** tested on the reference device.

### Changed — documentation
- `ARCHITECTURE.md` gained a scope section stating that it describes the
  user-mode translator and pointing to the feasibility analysis.
- `ROADMAP.md` gained a scope-correction section; Phase 9 gained a
  kernel-dependent-workload work item; Phase 1 gained an optional cross-build
  of the AArch64 UML port.
- `STATUS.md` rewritten to record the environment-persistence finding and the
  corrected scope.
- `.gitignore` covers UML experiment artifacts.

### Fixed — process
- Recorded that this environment **does not persist between sessions** and that
  stale `build/` artifacts from a previous session must not be mistaken for
  passing tests. All Phase 0 results were re-verified from a clean tree.
- `experiments/uml/run.sh` strips ANSI escapes before matching systemd target
  lines (systemd embeds colour codes inside target names, which silently broke
  the original pattern).

---

## [0.1.0] — 2026-10-19 — Phase 0: Project foundation

The first milestone: a buildable, testable, documented repository. **No guest
execution.** No instruction is decoded, interpreted or translated.

### Added — build system
- CMake ≥ 3.20 project (`CMakeLists.txt`), C++20, no compiler extensions.
- Compiler warning set (GCC/Clang) with optional `-Werror`.
- `CMakePresets.json` with `debug`, `release`, `sanitize`, `cross-aarch64`.
- AArch64 cross-compilation toolchain file
  (`cmake/toolchains/aarch64-linux-gnu.cmake`).

### Added — core library
- `hyperion::version_string()` and `hyperion::build_summary()` (`src/core/version.cpp`).
- Host capability probe (`src/core/host_probe.cpp`): architecture, OS, kernel,
  logical CPUs, page size, cache-line size, transparent hugepages, W^X
  expectation. Measures the host rather than assuming it (D-008).
- CLI (`src/cli/main.cpp`) with `version`, `probe`, `help`.

### Added — tests
- Dependency-free C++ test harness (`tests/harness/`) with registration,
  `CHECK`/`CHECK_EQ`, filtering and CTest integration (D-006).
- Unit tests for version and host probe (8 tests).
- Cross-architecture validation pipeline: AArch64 cross-build executed under
  `qemu-user`, enforced as the `cross.aarch64_tests` CTest test (D-007).

### Added — benchmarks
- Host benchmark harness (`benchmarks/host_bench.cpp`) with deterministic
  calibration and JSON output; guest-execution metrics explicitly marked
  unavailable (D-009).

### Added — tooling and environment
- `scripts/dev.sh` developer task runner; `scripts/bootstrap.sh` environment
  bootstrap.
- `.clang-format`, `.clang-tidy`, `.editorconfig`, `.gitignore`.
- GitHub Actions CI (`.github/workflows/ci.yml`).

### Added — documentation
- `README.md`, `ARCHITECTURE.md`, `ROADMAP.md`, `STATUS.md`, `DECISIONS.md`
  (D-001 … D-010), `COMPATIBILITY.md`, `BENCHMARKS.md`, `TESTING.md`,
  `CONTRIBUTING.md`, `SECURITY.md`, `THIRD_PARTY.md`, `CHANGELOG.md`.
- `docs/agent-handoff.md`, `docs/known-issues.md`, `docs/target-environment.md`,
  `docs/glossary.md`.

### Verified
- Native build: 12/12 targets, clean with `-Werror`.
- `unit.native` and `bench.smoke`: pass.
- `cross.aarch64_tests`: pass; AArch64 unit tests 8/8 under `qemu-aarch64`.
- AArch64 probe correctly reports `arch: aarch64` and `assume W^X: yes`.
- Pushed to `origin` as branch `phase-0-foundation`; GitHub Actions run
  `39244780989` (commit `460629a`) concluded `success` for all jobs: gcc build
  and tests, clang build and tests, AArch64 cross tests under `qemu-user`,
  ASan/UBSan, and clang-format. Pull request #1 opened against `main`.

### Known limitations
- No instruction decoding, interpretation or translation.
- Development host is x86-64; AArch64 validation is via `qemu-user` only.
- No reference-target (Snapdragon 855) measurements.
