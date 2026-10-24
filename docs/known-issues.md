# Known issues

Bugs, limitations and blocked work. Newest first within each section. An entry
is removed only when the issue is genuinely resolved, and the resolution is
noted in `CHANGELOG.md`.

---

## Blocked / cannot proceed without external input

| ID | Issue | Needs |
| --- | --- | --- |
| BLOCK-001 | No measurements from the reference target device (Snapdragon 855, Android 10). | Physical access to the device, or a comparable AArch64 Android host. Until then, all target-device claims are unverified. |
| BLOCK-002 | Development host is x86-64, so AArch64 code cannot be executed natively. | Mitigated by the cross + `qemu-user` pipeline (D-007). True AArch64 execution requires AArch64 hardware. |
| BLOCK-003 | The kernel-as-process tier cannot be committed to until the AArch64 UML port is run on the reference device. | AArch64 Android hardware. Until then the tier stays provisional (D-012) and no kernel-level code is written. |
| BLOCK-004 | The environment does not persist between sessions: a fresh container has no toolchain even if `build/` survives. | Run `scripts/bootstrap.sh`. Do not treat stale artifacts as evidence. See `STATUS.md` §2. |

---

## Limitations (by design, current phase)

| ID | Limitation | Notes |
| --- | --- | --- |
| LIM-002 | Syscall layer is partial: **128 of 375** numbers answered (122 implemented, 6 stubbed). | Phase 6. Counts are generated from `docs/syscalls/inventory.tsv` and enforced by `tools/gen_syscall_inventory.py --check`, so they cannot drift from the code. Still missing: sockets, `inotify_*`, `signalfd`, thread creation (`clone` with `CLONE_VM`/`CLONE_THREAD`), file-backed `mmap`, `waitid`. |
| LIM-017 | Five syscalls are **stubs**: `madvise`(28), `prctl`(157), `fadvise64`(221), `clock_getres`(229), `futex`(202). | They return success without doing the work, and the inventory says `stubbed` rather than `implemented`. A guest that *depends* on the effect will misbehave in a way that looks like an unrelated bug. (`rt_sigaction` and `sigaltstack` were in this list and are now real — see LIM-022.) Replacing the rest with real implementations or explicit refusals is the top remaining Phase 6 item. |
| LIM-022 | **Signal dispositions are recorded but signals are never delivered to guest handlers.** `rt_sigaction`(13) stores and reports `{handler, flags, restorer, mask}` per signal and `sigaltstack`(131) stores and reports the alternate stack, but nothing queues or dispatches a signal into guest code. | A guest that installs a handler and only queries it back now gets the truth; a guest that *waits* to be signalled still will not be. `SIGKILL`/`SIGSTOP` are correctly refused with `EINVAL`, `sigsetsize != 8` is `EINVAL`, bad pointers are `EFAULT`, and stack sizes below the kernel floor (2048 B) are `ENOMEM`. Delivery needs guest signal-frame construction and an arch-specific return trampoline, which is a separate project. |
| LIM-018 | ~~A TUI cannot put a terminal into raw mode~~ — **resolved**. | `ioctl` now implements `TCGETS`/`TCSETS`/`TCSETSW`/`TCSETSF` over the 36-byte asm-generic `struct termios`, plus `TIOCSWINSZ` and `FIONREAD`. `cfmakeraw()` followed by a read-back is verified over a real pseudo-terminal, both in a unit test and by a guest fixture run under `script(1)`. Remaining ioctl gaps are device-specific requests (`TIOCGPGRP`, `TIOCSPGRP`, `KD*`), which still return `-ENOTTY`. |
| LIM-019 | ~~Tier 1 and the AArch64 cross tree were not exercised for the syscall compatibility engine~~ — **resolved**. | With `g++-aarch64-linux-gnu` and the cached `qemu-aarch64-static`, `scripts/dev.sh cross-test` passes 10/10; the AArch64 unit suite is 344/0/2, and the compat guest runs at **tier 1** with `1412 basic block(s) translated`, `failures 0` and the same 170 308 guest steps as tier 0. The cross driver now also runs the compat suite and the registry CLI at both tiers, and fails if tier 1 falls back to the interpreter. Still true: these are `qemu-user` correctness results, not AArch64 hardware timings, and nothing has been run on the reference Android device. |
| LIM-020 | The registry is parsed once per run; there is no live reload during a guest run. | `SyscallDispatcher::reload()` swaps an immutable snapshot safely, and `--syscall-registry` is read at startup, but nothing re-reads the file while a guest executes. Editing the registry therefore requires restarting the guest. |
| LIM-021 | Concurrent `reload()` during guest execution is designed for but untested. | The snapshot/`shared_ptr` swap is unit-tested for consistency; no test runs syscalls concurrently with a reload. |
| LIM-003 | Cache invalidation is whole-cache epoch invalidation rather than fine-grained per-page invalidation. | Writing to an executable region or running `mprotect`/`munmap` bumps `Memory::code_epoch()` so neither Tier 0 nor Tier 1 ever executes stale translations; per-page invalidation is deferred until multi-megabyte JIT guests require it. |
| LIM-004 | `qemu-user` validates correctness, not hardware timing. | Relative speedups on identical hosts are recorded in `BENCHMARKS.md` §4.2.8; never quote `qemu-user` numbers as Snapdragon 855 hardware performance. |
| LIM-005 | `hyperion_bench` calibrates the host; guest tier timings live in `hyperion_tier_bench`. | Run `hyperion_tier_bench` (native) and `scripts/bench-tier-qemu.sh` (cross AArch64) for Tier 0 vs Tier 1 comparisons (`BENCHMARKS.md` §4.2.8). |
| LIM-006 | No Windows/macOS guest support. | Out of scope, permanently. |
| LIM-007 | Target device has no KVM and no root; the design must not assume either. | Enforced as a project rule. |
| LIM-008 | **Kernel-level compatibility is investigated, not implemented.** | The user-mode translator cannot provide `systemd` as PID 1, containers, or kernel-dependent services. The kernel-as-process tier is provisional and unvalidated on the target (D-011, D-012). |
| LIM-009 | AArch64 UML is an **out-of-tree** dependency (38 unmerged commits). | If the tier is adopted, it carries a maintenance liability (R7 in `docs/architecture-feasibility.md`). |
| LIM-010 | **Memory loads/stores and `push`/`pop` are Tier 0-only (`LEA` is translated in both Tier 0 and Tier 1).** | Tier 0 executes all ModRM/SIB/`[rip+disp32]` memory operands and `push`/`pop`. Tier 1 translates `LEA` (`[rip+disp32]` and scaled SIB) natively, while memory load/store operands and `push`/`pop` cut blocks cleanly to Tier 0 until Phase 7 soft-MMU lands. |
| LIM-011 | **No cross-block register allocation.** | Register allocation keeps hottest guest registers in `x19..x28` across entire blocks and multi-iteration self-loop superblocks, spilling at cold block exits. |
| LIM-012 | **Block chaining is 2-way inline C++ dispatch + self-loop superblocks rather than cross-block native jump patching.** | Self-loops stay entirely in generated ARM64 code with zero dispatcher transitions; multi-block loops chain via inline 2-way `chain_block0/1` pointers without cache hash lookups. Direct cross-block `B` patching is a future Phase 5 option. |
| LIM-013 | **Step-limit granularity across non-self-loop multi-instruction blocks.** | Self-loop superblocks track exact iteration budgets; non-loop blocks check `max_steps` at block entry so Tier 1 never exceeds `max_steps`. |
| LIM-014 | `qemu-user` runs generated code, but it is **not the reference device**. | No measurement exists on the Snapdragon 855 / Android 10 target. |
| LIM-015 | **`*at` path syscalls currently require `AT_FDCWD`.** | `openat`/`newfstatat`/`readlinkat`/`statx` return `-ENOSYS` on explicit directory fds (counted in `unimplemented_syscalls`). `dup`/`dup2`/`dup3`/`fcntl`/`pread64`/`pwrite64` are now implemented over real kernel file descriptors. |
| LIM-016 | **The address-space memory bound is per-mapping, not total.** | A single `mmap` larger than `kDefaultMaxMapBytes` (1 GiB) is refused with `-ENOMEM` (D-029), but many mappings each under the cap can still sum large. A total-commit limit is Phase 10 work (SECURITY.md R5). |

---

## Correctness bugs

No open correctness bugs. Recorded history:

| ID | Symptom | Status |
| --- | --- | --- |
| BUG-001 | 64-bit `ADD` never set carry (`sum > mask` is impossible when the mask is `~0`). Found by the hardware differential. | Fixed (Phase 2). |
| BUG-002 | `Jcc` short jumps (`0x70..0x7F`) were decoded as `rel32`, producing wild targets. Found by the hardware differential. | Fixed (Phase 2). |
| BUG-003 | A block whose *first* instruction was unsupported was emitted as an empty fall-through, so the dispatcher looped forever. | Fixed (Phase 4); `translator.dispatcher_stops_at_unsupported` guards it. |
| BUG-004 | The code-fetch window shrank only to 15 bytes, so a program image shorter than that could not be translated. | Fixed (Phase 4); the CLI smoke test guards it. |
| BUG-005 | The dispatcher checked the step limit before running a block and then added the whole block count, so a 1,000,000-step run could retire 1,000,001 instructions. Found by the nested-loop differential. | Fixed (Phase 4); the step bound is now hard. |

When a bug is found, record: ID, symptom, minimal reproducer, affected commit,
severity (correctness/performance/cosmetic), status, and the test that guards
against regression.

---

## Toolchain and environment issues

| ID | Issue | Status |
| --- | --- | --- |
| ENV-001 | `g++-aarch64-linux-gnu` is required in addition to `gcc-aarch64-linux-gnu`; installing only the C cross-compiler breaks the cross test with "CXX compiler not found". | Resolved in `scripts/bootstrap.sh`, which installs both. |
| ENV-002 | ASan interacts with custom executable code caches. | Not yet encountered (no code cache). Expected in Phase 3; document per-test exclusions when they arise. |
| ENV-003 | CI prints a Node.js 20 deprecation annotation on every job: `actions/checkout@v4` is forced to run on Node.js 24. | Cosmetic; jobs pass. Resolve by bumping the pinned action version when a Node-24 action release is available. |

---

## Benchmarking defects

| ID | Issue | Status |
| --- | --- | --- |
| BENCH-001 | The Phase 1 benchmark harness reported a **fabricated** result. `-O2` hoisted the loop-invariant calibration call out of the repeat loop, so every timed region collapsed to ~0.00003 ns/op (~30 ps per iteration — far below one clock cycle) while the checksum stayed correct, so the error was silent. Detected by checking that total time scales with `--iterations`. | **Fixed.** An `asm` optimisation barrier now keeps the work between the clock reads, and the harness rejects an implausible timing (faster than one cycle per iteration) instead of reporting it. Regression test `bench.timing_not_eliminated`. |

---

## Rejected optimization candidates

Recorded so a future agent does not repeat a dead end.

| ID | Candidate | Verdict |
| --- | --- | --- |
| OPT-001 | Clear CF/OF/AF in `emit_flags_logical` by storing the AArch64 **zero register** (`strb wzr, …`) instead of `movz kT1, #0` + three `strb`s. | **Accepted & implemented.** Earlier rejection tested the invalid register name `w31`; the architectural mnemonic `strb wzr, [x8, #136]` encodes to `0x3902211f` (verified against `aarch64-linux-gnu-as` and `qemu-aarch64`), eliminating `movz` cleanly. |
| OPT-002 | Invert PF in `emit_parity` without a scratch `movz + eor` pair. | **Accepted & implemented.** Emitting `mvn w, w` (`ORN Rd, WZR, Rm`, `0x2a2003e0`) immediately before `ubfx w, w, #0, #1` shrinks parity computation from 7 to 5 AArch64 instructions with zero logical-immediate encoder complexity. |

---

## Open questions

| ID | Question | Impact |
| --- | --- | --- |
| Q-001 | Which upstream components, if any, should be reused rather than built? | Phase 1 decides; affects everything downstream. |
| Q-002 | Custom IR versus direct translation for the first code generator? | Phase 1/2; see D-004 for the current lean. |
| Q-003 | How to model x86 TSO memory ordering on weakly ordered AArch64? | Phase 7; affects correctness of multithreaded guests. |
| Q-004 | Does the AArch64 UML port build and boot under Android 10 on the Snapdragon 855, with no root and no KVM? | Decides whether the kernel-as-process tier is real (R1/R2). Blocks any kernel-level code. |
