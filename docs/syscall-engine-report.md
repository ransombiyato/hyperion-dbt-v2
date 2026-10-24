# Dynamic Syscall Translation and Compatibility Engine — final report

Project HYPERION · branch `main` · HEAD `9d3bdcc`
Host used for every measurement below: **x86-64**, Debian 13, kernel 6.1.158,
GCC 14.2.0, CMake 4.4.4, Ninja 1.13.2, clang-format 19.1.7.

> **Read this first.** The development host is x86-64, so AArch64 code was
> validated by cross-compiling with `aarch64-linux-gnu-g++` 14.2.0 and running
> under the repo-cached `qemu-aarch64-static`. **Tier 1 (the AArch64 JIT) is
> exercised and passes** — see §6a. What is *not* covered: real AArch64 hardware
> timings (qemu-user is a correctness tool, never a performance one) and the
> reference Android device. Performance numbers in §7 are host-CPU only.

---

## 1. Architecture summary

A guest `syscall` instruction reaches **one** entry point,
`MinimalSyscallHandler::handle()` (`src/core/syscall.cpp`). It was extended, not
replaced: the static handler it already had is still the implementation for most
syscalls, and the new layers sit around it in a fixed order.

| # | Layer | Cost when it answers | Implemented in |
| --- | --- | --- | --- |
| 1 | Compiled rule table — bounds check + one array index (`kMaxSyscall = 512`) | constant time, no allocation | `syscall_dispatch.cpp` |
| 2 | Existing static switch (`handle_unguarded()`) | jump table | `syscall.cpp` (unchanged) |
| 3 | Existing legacy `linux_compat_` switch | only for numbers layer 2 does not claim | `syscall.cpp` (unchanged) |
| 4 | Refusal: `-ENOSYS`, counted | cheapest of all (measured: ~10 ns) | `syscall.cpp` |

Design rules that were enforced, not just stated:

- **No JSON parsing, file I/O, lock, allocation or string hashing on the hot
  path.** The registry is parsed once per process by `SyscallDispatcher::reload()`
  into an immutable `CompiledRules` snapshot held in a `shared_ptr`; dispatch only
  reads it. A reload publishes a *new* snapshot, so a syscall in flight keeps the
  table it started with.
- **The static implementation always wins.** A rule for a syscall HYPERION
  already implements is rejected at compile time with a diagnostic, so tested
  code can never be shadowed by configuration.
- **No competing dispatcher and no duplicated handlers.** The new layer holds
  three things only: a compiled table, a whitelist of adapters, and the alias
  argument rewrite. Adapter bodies live in `syscall_adapters.cpp`; nothing in
  `handle_unguarded()` was rewritten.
- **A guest syscall number is never forwarded to the host kernel.** A rule can
  name a compiled, tested adapter by id — never a host syscall number, library,
  shell command or expression. An unknown handler name is a validation error.
- **`fork`ed children inherit the parent's compiled rules**
  (`SyscallDispatcher::adopt_snapshot`). Without this a user rule worked before
  `fork(2)` and silently became `-ENOSYS` after it.
- **Failure is never fatal.** A missing, unreadable, corrupt or newer-schema
  registry leaves the built-in rules in force and cannot stop startup; reading
  never rewrites the file.

## 2. Resolved registry path and override

| Priority | Source |
| --- | --- |
| 1 | `--syscall-registry=FILE` (CLI) |
| 2 | `$HYPERION_SYSCALL_REGISTRY` |
| 3 | `$XDG_DATA_HOME/hyperion/syscalls/registry.json` |
| 4 | `$HOME/.local/share/hyperion/syscalls/registry.json` (same on Termux/Android) |

`hyperion syscall-registry --list` prints the chosen path *and* the reason:

```
registry path  : /home/user/.local/share/hyperion/syscalls/registry.json
resolved from  : default ($HOME/.local/share)
```

`HYPERION_NO_SYSCALL_REGISTRY=1` and `--no-syscall-registry` mean "ignore the
file, use the compiled-in rules" — they do **not** disable the compatibility
layer, and they create nothing.

Writes are atomic (temp file + `fsync` + `rename` + directory `fsync`, previous
contents kept as `.bak`), files are `0600`, directories `0700`, reads are capped
at 16 MiB, and v1 documents are migrated to v2 in memory on read.

## 3. JSON schema and a real example entry

Schema version 2 (`kRegistrySchemaVersion`); v1 = the same document without
`origin`. Unknown fields are ignored; unknown enum values, out-of-range numbers,
duplicate ids/numbers and unknown handler names are validation errors.

Top level: `schema_version`, `guest_arch`, `host_arch`, `created_unix`,
`updated_unix`, `comment`, `rules[]`.

Per rule: `id`, `name`, `syscall`, `implementation`
(`builtin_adapter` | `guest_alias`), `handler`, `transforms[]`, `target_syscall`,
`args[]`, `return_transform` (must be `errno_negative`), `requirements[]`,
`rule_version`, `origin` (`builtin` | `package` | `user`), `origin_detail`,
`enabled`, `notes`.

A shipped default, verbatim from `hyperion syscall-registry --export-defaults`:

```json
{
  "id": "hyperion.default.poll",
  "name": "poll",
  "syscall": 7,
  "implementation": "builtin_adapter",
  "handler": "compat.poll",
  "transforms": ["guest_fd", "layout.pollfd", "sigmask.u64"],
  "return_transform": "errno_negative",
  "rule_version": 1,
  "origin": "package",
  "origin_detail": "generated from the compiled-in adapter table",
  "enabled": true,
  "notes": "poll(2) over an arbitrary set of guest descriptors"
}
```

A user rule, taken from the test suite — it makes syscall 300 (unassigned on
x86-64) behave as `getpid(2)`, and the guest test proves it works and that
deleting it restores `-ENOSYS`:

```json
{
  "schema_version": 2,
  "guest_arch": "x86_64",
  "rules": [
    {
      "id": "user.test.custom300",
      "name": "custom300",
      "syscall": 300,
      "implementation": "guest_alias",
      "target_syscall": 39,
      "origin": "user",
      "rule_version": 1,
      "enabled": true,
      "notes": "syscall 300 -> getpid(2); there is no such x86-64 syscall"
    }
  ]
}
```

## 4. Newly supported syscalls, with status

Counts are generated from `docs/syscalls/inventory.tsv` and enforced in both
directions by `tools/gen_syscall_inventory.py --check`.

**128 of 375** numbers are answered: **121 implemented, 7 stubbed**. Before this
work the inventory claimed 47.

Newly `implemented` in this work (42 numbers):

`poll`(7) · `select`(23) · `sched_yield`(24) · `getitimer`(36) · `setitimer`(38)
· `fsync`(74) · `fdatasync`(75) · `truncate`(76) · `ftruncate`(77) · `chdir`(80)
· `fchdir`(81) · `umask`(95) · `getrlimit`(97) · `getrusage`(98) · `times`(100)
· `setpgid`(109) · `getpgrp`(111) · `setsid`(112) · `getpgid`(121) ·
`sched_getparam`(143) · `sched_setscheduler`(144) · `sched_getscheduler`(145) ·
`sched_get_priority_max`(146) · `sched_get_priority_min`(147) · `setrlimit`(160)
· `sync`(162) · `epoll_wait`(232) · `epoll_ctl`(233) · `linkat`(265) ·
`symlinkat`(266) · `pselect6`(270) · `ppoll`(271) · `epoll_pwait`(281) ·
`timerfd_create`(283) · `timerfd_settime`(286) · `timerfd_gettime`(287) ·
`eventfd2`(290) · `epoll_create1`(291) · `syncfs`(306) · `memfd_create`(319) ·
`membarrier`(324) · `epoll_pwait2`(441)

Plus `link`(86) and `symlink`(88), added after a guest fixture showed `ln`
failing under translation, and `TCGETS`/`TCSETS`/`TCSETSW`/`TCSETSF`/
`TIOCSWINSZ`/`FIONREAD` inside the existing `ioctl`(16). 39 further numbers were
already answered by the legacy compat switch but were mislabelled `not-started`
in the inventory (`open`, `stat`, `lstat`, `rt_sigprocmask`, `ioctl`, `pipe`,
`mremap`, `kill`, `rename`, `mkdir`, `rmdir`, `unlink`, `chmod`, `fchmod`,
`sysinfo`, `statfs`, `fstatfs`, `futex`, `sched_getaffinity`, `mkdirat`,
`unlinkat`, `renameat`, `fchmodat`, `faccessat`, `utimensat`, `pipe2`,
`renameat2`, `faccessat2`, …) — that correction is why the roadmap had been
pointed at work that was already done.

- **Moved from stub to real:** `nanosleep`(35), `clock_nanosleep`(230).
- **Still stubbed, and labelled `stubbed` rather than `implemented`:**
  `rt_sigaction`(13), `madvise`(28), `sigaltstack`(131), `prctl`(157),
  `fadvise64`(221), `clock_getres`(229), and `futex`(202).
- **`futex` was corrected *downwards* in this work.** It had been counted as
  implemented while returning success from `FUTEX_WAIT` without blocking — i.e.
  telling callers they had been woken when nothing had happened. It now does the
  real value comparison (`-EAGAIN`/`-EFAULT`), reports `WAKE` truthfully as zero
  woken, and refuses the paths that need a wait queue with `-ENOSYS`. A stub that
  lies is worse than a refusal, and the inventory now says `stubbed`.

## 5. Changed files

New:

```
include/hyperion/json.hpp                 include/hyperion/syscall_detail.hpp
include/hyperion/syscall_dispatch.hpp     include/hyperion/syscall_registry.hpp
src/core/json.cpp                         src/core/syscall_dispatch.cpp
src/core/syscall_adapters.cpp             src/core/syscall_registry.cpp
benchmarks/syscall_bench.cpp
tests/unit/test_json.cpp                  tests/unit/test_syscall_registry.cpp
tests/unit/test_syscall_dispatch.cpp      tests/unit/test_syscall_adapters.cpp
tests/cli/registry_cli_test.sh
tests/guest/syscall_compat_test.sh
tests/guest/fixtures/compat_syscalls.c
tests/guest/fixtures/syscall_custom.c
tests/guest/fixtures/fork_rule_inherit.c
```

Modified (source): `CMakeLists.txt`, `include/hyperion/syscall.hpp`,
`include/hyperion/process.hpp`, `src/core/syscall.cpp`, `src/core/process.cpp`,
`src/cli/main.cpp`, `scripts/hyperion`, `install.sh`,
`tools/gen_syscall_inventory.py`, `tests/CMakeLists.txt`,
`benchmarks/CMakeLists.txt`, `tests/unit/test_syscall.cpp`.

Modified (docs): `README.md`, `ARCHITECTURE.md`, `COMPATIBILITY.md`, `STATUS.md`,
`ROADMAP.md`, `TESTING.md`, `CHANGELOG.md`, `docs/known-issues.md`,
`docs/syscalls/README.md`, `docs/syscalls/inventory.tsv`,
`docs/syscalls/U1..U4.md` (regenerated).

## 6a. AArch64 / Tier 1 verification (measured)

```
sudo apt-get install -y --no-install-recommends g++-aarch64-linux-gnu
scripts/dev.sh cross-test        # 10/10 passed
```

| Run | Result |
| --- | --- |
| AArch64 unit suite (`qemu-aarch64-static`) | **342 passed, 0 failed, 2 skipped** (of 344 registered) |
| AArch64 differential suite | 0 passed — correct: no x86-64 oracle on AArch64, so every case SKIPs |
| `cross.aarch64_tests` (whole pipeline) | passed, 18–61 s |
| AArch64 guest **process** tests, tier 0 / tier 1 | passed / passed |
| AArch64 **syscall compat** suite, tier 0 / tier 1 | 11/11 checks each |
| AArch64 registry CLI | 11/11 checks |
| Tier 1 JIT evidence | `tier 1: 1412 basic block(s) translated`, `failures 0`, **170 308 guest steps — identical to tier 0** |

The step-count identity matters: the JIT and the interpreter agree instruction
for instruction on a guest that exercises 20 adapter-handled syscalls, `poll`,
`epoll`, `select`, `timerfd`, `nanosleep`, `getrusage` and friends.

The cross driver did **not** cover this layer before; `tests/run_cross_tests.cmake`
now runs the compat suite at both tiers, runs the registry CLI under qemu, and
**fails the build if tier 1 falls back to the interpreter** (`tier 1: N basic
block(s) translated`, `N > 0`, plus a "must not print a fallback warning" check).
Without that assertion a silent fallback would pass every behavioural check.

## 6. Build, test and measurement results (actual)

```sh
cmake -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/dev -j8            # clean, no warnings
ctest --test-dir build/dev             # 16/16 passed
./build/dev/tests/hyperion_unit_tests  # 316 passed, 0 failed, 24 skipped (of 340)
python3 tools/gen_syscall_inventory.py --check   # 0 errors, 0 warnings
```

| Suite | Command | Result |
| --- | --- | --- |
| Native unit | `./build/dev/tests/hyperion_unit_tests` | **316 passed, 0 failed, 24 skipped** (of 340) |
| Full CTest tree | `ctest --test-dir build/dev` | **16/16 passed** |
| ASan + UBSan | `cmake -B build/asan -DHYPERION_ENABLE_SANITIZERS=ON` + build + `ctest` | **16/16 passed**, no report |
| Guest compatibility | `ctest -R guest.syscall_compat` | 11 checks, 27 guest-side assertions |
| Registry CLI contract | `ctest -R cli.syscall_registry` | 11 checks |
| Formatting | clang-format 19.1.7 `--dry-run --Werror` | clean |
| Lint | `shellcheck` on all touched scripts | clean |
| Inventory gate | `gen_syscall_inventory.py --check` | OK |

End-to-end, a real static x86-64 guest using the adapted syscalls:

```
$ HYPERION_SYSCALL_STATS=1 hyperion run --tier=0 compat_syscalls
… failures 0
status: ok
steps:  170659
syscall dispatch: adapter=20 alias=0 existing=37 unsupported=0 translation_failures=0
registry: /tmp/final/registry.json (40 adapter rule(s), 2 alias rule(s))
```

## 7. Performance measurements

`benchmarks/hyperion_syscall_bench.cpp` (target `hyperion_syscall_bench`,
CTest `syscall_bench.smoke`). Method: warm-up sample discarded, median and IQR of
the rest, optimisation barrier, per-sample checksum, BENCH-001 plausibility
floor; `--repeats 1` is refused rather than publishing a one-sample "spread".

Measured 2026-10-24, 300 000 iterations/sample, 9 samples — two runs shown
because this container's clocks are noisy:

| Target | run A median ns | run B median ns | what |
| --- | --- | --- | --- |
| `host_getpid` | 121.0 | 101.3 | bare host `getpid(2)` |
| `host_sched_yield` | 221.6 | 180.6 | bare host `sched_yield(2)` |
| `direct_adapter` | 224.1 | 208.6 | adapter called directly, no dispatch |
| `adapter_dispatch` | 237.4 | 225.6 | `handle()` → table → adapter |
| `existing_dispatch` | 133.7 | 124.4 | `handle()` → static switch |
| `unsupported` | 12.9 | 9.7 | `handle()` → `-ENOSYS` + counting |

**Table overhead of layer 1: 13–17 ns per call** — smaller than the syscall it
dispatches to. The refusal path is the cheapest, so an unsupported syscall cannot
become a hot spot (it does no I/O; the reporting happens once, at the end).

**Limitations of these numbers:** host CPU only, dispatch layer only. They
exclude guest instruction translation entirely and say nothing about an AArch64
device. They must not be quoted as end-to-end performance.

## 8. Bugs this work found and fixed

1. **Registry rules could be silently dropped while `--validate` exited 0.**
2. **`--json` printed the file's rules, not the effective set** (1 rule shown
   where 43 were active) and said nothing about rejections.
3. **Diagnostics printed twice.**
4. **Stack buffer overflow in the `poll` adapter** — 8-byte read out of the
   `events` field of an 8-byte `struct pollfd`; invisible to every normal build,
   caught by ASan.
5. **`install.sh` ignored the registry entirely.**
6. **`nanosleep` was a stub returning success instantly** → guest retry loops spun.
7. **`link`(86)/`symlink`(88) were missing** → `ln` failed under translation.
8. **A forked child lost the parent's registry rules.**
9. **Inventory over-reported missing work by 39 syscalls.**
10. **A terminal application could not enter raw mode** (`TCSETS` missing).

## 9. Remaining unsupported cases and risks

| Area | State | Risk |
| --- | --- | --- |
| Threads (`clone` with `CLONE_VM`/`CLONE_THREAD`) | refused with `-ENOSYS` by design | correct, but any threaded guest does not run |
| `futex`(202) | **stub by design**: value comparison real, blocking refused | cannot be fixed before thread creation exists; a guest that contends will get a clean `-ENOSYS` rather than a false wake-up |
| `rt_sigaction`, `sigaltstack`, `prctl`, `madvise`, `fadvise64`, `clock_getres` | stubs that report success | **worst category**: a guest that depends on the effect misbehaves later, looks like an unrelated bug. Rust's stack-overflow guard checks that `sigaltstack` took effect |
| Sockets | not started | blocks an editor that spawns a language server |
| `signalfd`, `inotify_*` | not started | blocks file-watching |
| File-backed `mmap`, `waitid`, `openat2` | not started | |
| Registry live reload during a run | parsed once at startup; `reload()` is safe but nothing re-reads mid-run | editing the registry requires restarting the guest |
| Concurrent `reload()` vs syscalls | designed for (immutable snapshot) and unit-tested for consistency, but no test runs them concurrently | unproven concurrency |
| **Tier 1 / AArch64** | **exercised and passing** (§6a) | remaining gap is real hardware: `qemu-user` cannot measure performance and nothing has run on the reference Android device |

Adapters assume the host kernel accepts the request; a host that rejects one
(e.g. `membarrier` returning `EPERM`) surfaces as a normal `-errno` from the
adapter after `errno` has deliberately not been touched.

## 10. Handoff note — how to work on this next

1. **Read `ARCHITECTURE.md` §6 and `COMPATIBILITY.md` §5.06 first.** They state
   the layer order and every structure/flag translation, which is the map for
   anything you touch.
2. **Never add a syscall by special-casing it in `handle_unguarded()` if the ABI
   differs.** Write an adapter in `src/core/syscall_adapters.cpp`, add a rule to
   `default_registry_document()`, and declare the `transforms` you perform —
   the compiler and the tests check that declaration against the compiled list.
3. **A stub must carry `[stub]` in `is_linux_compat_syscall()`** and the
   inventory gate then *requires* its status to be `stubbed`. This exists so a
   stub cannot be counted as support.
4. **Update `docs/syscalls/inventory.tsv`, then run
   `python3 tools/gen_syscall_inventory.py`.** `--check` fails the build if the
   source and the table disagree in either direction. Update the counts in
   `README.md`, `STATUS.md` and `scripts/hyperion status` when the total changes.
5. **Add a test at the right level**: unit test for the translation, guest
   fixture assertion if a real program reaches it, CLI assertion if the user
   sees it. Three suites exist because each one caught something the others did
   not.
6. **Run the ASan build.** It found the only memory-safety bug in this work, in
   code that every ordinary build passed.
7. **Verify non-vacuity deliberately.** When a fix makes a test pass, disable the
   fix once and confirm the test fails (that is how `fork_rule_inherit.c` was
   validated: with the fix removed the guest prints `parent=1 child=0`).
8. **Highest-value next items, in order:** `rt_sigaction` + `sigaltstack`
   (replace the lying stubs), `futex` validation, `inotify_*`/`signalfd`,
   sockets. **Keep the cross tree green**: `scripts/dev.sh cross-test` runs the
   compat suite at both tiers and fails if tier 1 falls back, so it is the guard
   that stops AArch64 support silently rotting.
