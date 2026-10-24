# Dynamic Syscall Translation and Compatibility Engine — final report

Project HYPERION · branch `main` · last code commit `0197a55` (documentation
commits follow it; `git log --oneline -5` shows them)
Host used for every measurement below: **x86-64**, Debian 13, kernel 6.1.158,
GCC 14.2.0, CMake 4.4.4, Ninja 1.13.2, clang-format 19.1.7.

> **Read this first.** The development host is x86-64, so AArch64 code was
> validated by cross-compiling with `aarch64-linux-gnu-g++` 14.2.0 and running
> under the repo-cached `qemu-aarch64-static`. **Tier 1 (the AArch64 JIT) is
> exercised and passes** — see §6a. What is *not* covered: real AArch64 hardware
> timings (qemu-user is a correctness tool, never a performance one) and the
> reference Android device. Performance numbers in §7 are host-CPU only.
>
> **Latest corrections:** `futex` no longer reports wake-ups that never happened
> (`stubbed`); `rt_sigaction`/`sigaltstack` record and report the guest's real
> signal state instead of fabricating `SIG_DFL`; `clock_getres`/`fadvise64` are
> translated rather than stubbed; `madvise`/`prctl` stay stubs with the reason in
> the source. Inventory: **127 implemented + 1 stubbed = 128 answered of 375**.

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

**128 of 375** numbers are answered: **127 implemented, 1 stubbed**. Before this
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

- **Moved from stub to real:** `nanosleep`(35), `clock_nanosleep`(230),
  `rt_sigaction`(13), `sigaltstack`(131), `clock_getres`(229), `fadvise64`(221),
  `madvise`(28), `prctl`(157).
- **Still stubbed, and labelled `stubbed` rather than `implemented`:**
  `futex`(202) — and it says *why* in the source. `madvise`(28) and `prctl`(157),
  which were in this list, are now implemented (§6d).
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

## 6. Build, test and measurement results (actual)

```sh
cmake -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/dev -j8            # clean, no warnings
ctest --test-dir build/dev             # 16/16 passed
./build/dev/tests/hyperion_unit_tests  # 323 passed, 0 failed, 24 skipped (of 347)
python3 tools/gen_syscall_inventory.py --check   # 0 errors, 0 warnings
```

| Suite | Command | Result |
| --- | --- | --- |
| Native unit | `./build/dev/tests/hyperion_unit_tests` | **323 passed, 0 failed, 24 skipped** (of 347) |
| Full CTest tree | `ctest --test-dir build/dev` | **16/16 passed** |
| ASan + UBSan | `cmake -B build/asan -DHYPERION_ENABLE_SANITIZERS=ON` + build + `ctest` | **16/16 passed**, no report |
| Guest compatibility | `ctest -R guest.syscall_compat` | 11 checks, 43 guest-side assertions |
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

## 6d. `madvise`, `prctl`, and the futex timeout

The last three stubs. Two could be implemented properly once the memory model was
looked at, and the third turned out to be half-implementable for a reason that is
worth writing down.

**`madvise`(28).** Guest memory here is always anonymous, private, host-heap
storage owned by `GuestAddressSpace` — there are no file-backed or shared
mappings (`COMPATIBILITY.md`). That makes the advice modes split three ways, and
each third gets the answer it deserves:

| Modes | What is done | Why that is the truth |
| --- | --- | --- |
| `MADV_NORMAL`/`RANDOM`/`SEQUENTIAL`/`WILLNEED`/`HUGEPAGE`/`NOHUGEPAGE`/`DONTDUMP`/`DODUMP`/`COLD`/`PAGEOUT` | range probed, return 0 | pure hints; the kernel promises nothing a guest can test |
| `MADV_DONTNEED` | **the range is zeroed** | for anonymous private memory this *is* the entire documented effect, so it is implemented, not approximated |
| `MADV_FREE` | 0, contents kept | the kernel may discard only lazily; keeping them is permitted |
| `MADV_DONTFORK`/`DOFORK`/`KEEPONFORK`/`WIPEONFORK` | `-ENOSYS` | per-mapping state this model does not carry; a guest that asked for `DONTFORK` and then forked would otherwise find the mapping still there |
| `MADV_REMOVE` | `-EINVAL` | the kernel's own answer for a private anonymous mapping |
| unknown advice | `-EINVAL` | same as the kernel |
| unmapped range / misaligned address / zero length | `-ENOMEM` / `-EINVAL` / 0 | same as the kernel |

**`prctl`(157).** A documented subset is honoured, and the rest is refused with a
real errno rather than a fake success:

| Option | Behaviour |
| --- | --- |
| `PR_SET_NAME`/`PR_GET_NAME` | stored (15 bytes + NUL, as the kernel truncates) and reported back |
| `PR_SET_PDEATHSIG`/`PR_GET_PDEATHSIG` | recorded and reported. The host process is deliberately **not** armed: a guest's parent is not HYPERION's parent, so arming it would fire on the wrong event |
| `PR_SET_DUMPABLE`/`PR_GET_DUMPABLE` | recorded (default 1) and reported |
| `PR_SET_TIMERSLACK`/`PR_GET_TIMERSLACK` | **forwarded to the host**, so the answer is the real slack this process uses |
| `PR_SET_NO_NEW_PRIVS`/`PR_GET_NO_NEW_PRIVS` | **forwarded to the host** - one-way and strictly more restrictive, so it is the conservative direction |
| `PR_SET_SECCOMP`, `PR_SET_MM`, `PR_SET_MCE_KILL`, child-subreaper, `PR_CAPBSET_*` | `-ENOSYS`: they would need to change this process's privileges or install a filter over guest execution, which is not modelled |
| anything else | `-EINVAL`, as the kernel does |

**`futex`(202) with a timeout.** Nothing in this execution model can ever wake a
waiter — `clone(CLONE_VM|CLONE_THREAD)` is refused, so no second thread exists to
call `FUTEX_WAKE`. But a waiter with a *relative timeout* has a well-defined
answer even in that world: it sleeps and reports `ETIMEDOUT`, which is exactly
what `pthread_cond_timedwait` and every bounded retry loop expect. That is now
implemented with a real `nanosleep`; the value comparison still runs first
(`-EAGAIN` on a changed word, `-EFAULT` on an unmapped address), an invalid
`timespec` is `-EINVAL`, and a wait with **no** timeout still returns `-ENOSYS`
because blocking forever would hang a program that a real kernel would also leave
blocked, and returning 0 would be the false wake-up this layer was fixed to stop
telling. The inventory therefore still says `stubbed` — the one remaining entry.

## 6a. AArch64 / Tier 1 verification (measured)

```
sudo apt-get install -y --no-install-recommends g++-aarch64-linux-gnu
scripts/dev.sh cross-test        # 10/10 passed
```

| Run | Result |
| --- | --- |
| AArch64 unit suite (`qemu-aarch64-static`) | **349 passed, 0 failed, 2 skipped** (of 351 registered) |
| AArch64 differential suite | 0 passed — correct: no x86-64 oracle on AArch64, so every case SKIPs |
| `cross.aarch64_tests` (whole pipeline) | passed, 18–61 s |
| AArch64 guest **process** tests, tier 0 / tier 1 | passed / passed |
| AArch64 **syscall compat** suite, tier 0 / tier 1 | 11/11 checks, 43 guest assertions each |
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

## 6b. Signal-state fidelity: `rt_sigaction` and `sigaltstack`

Both numbers used to be listed as stubs that answered success and did nothing.
They are the same class of defect the `futex` correction fixed: the caller is
told something happened that did not.

**What was wrong.** `rt_sigaction`(13) wrote 32 zero bytes into the caller's
`oldact` and returned 0. A program that installs `SIGSEGV` and then asks what is
installed was told `SIG_DFL` — the opposite of the truth. `sigaltstack`(131) did
the same with a 24-byte `stack_t`, which is exactly what Rust's standard library
reads back to confirm its stack-overflow guard is armed.

**What it does now** (`src/core/syscall.cpp`, state in
`MinimalSyscallHandler::sigactions_` / `altstack_*_`):

| Call | Behaviour |
| --- | --- |
| `rt_sigaction(sig, act, oldact, 8)` | Stores `{handler, flags, restorer, mask}` per signal 1..64; writes the previous action to `oldact` only when `oldact != 0`; a never-set signal reports the kernel's `SIG_DFL` (all zeros) |
| `rt_sigaction` invalid input | `sig < 1` or `> 64` → `-EINVAL`; `SIGKILL`/`SIGSTOP` → `-EINVAL` (the kernel refuses, and a guest believing it ignored a fatal signal is dangerous); `sigsetsize != 8` → `-EINVAL`; either pointer outside the guest address space → `-EFAULT` (via the existing memory validation, not a new check) |
| `sigaltstack(ss, old_ss)` | Stores `{ss_sp, ss_flags, ss_size}`; reports `SS_DISABLE` before anything is installed; `SS_DISABLE` clears it again; `SS_AUTODISARM` accepted |
| `sigaltstack` invalid input | undefined flag bits → `-EINVAL`; `ss_size` below the kernel's 2048-byte floor → `-ENOMEM`; bad pointers → `-EFAULT` |

**What is still not implemented, stated plainly (LIM-022):** nothing queues or
delivers a signal into guest code. There is no guest signal frame, no
arch-specific return trampoline, no interaction between the mask and delivery.
The disposition is *recorded and reported*; it is not *acted upon*. That is a
truthful capability, not a claim of signal support.

**Tests.** Three unit tests (`linux_compat_rt_sigaction_round_trips_the_disposition`,
`..._rejects_what_the_kernel_rejects`, `linux_compat_sigaltstack_records_the_alternate_stack`)
including set-then-get round-trip, per-signal independence, `SIG_IGN` as a real
value, `SIG_DFL` default, and every `-EINVAL`/`-EFAULT`/`-ENOMEM` path. The guest
fixture gained `sigaltstack`, `sigaction`, `sigaction_per_signal` and
`sigaction_rejects_sigkill`, so real glibc calls are exercised through the full
dispatch path at both tiers. Non-vacuity was checked the same way as before: with
the store removed the round-trip assertion fails.

## 6c. Two more stubs replaced with real translations

`clock_getres`(229) and `fadvise64`(221) also answered success while doing
nothing. Neither needs a wait queue or a signal frame, so both are now real:

| Call | Behaviour |
| --- | --- |
| `clock_getres(clockid, res)` | Asks the **host** `clock_getres` and writes the 16-byte guest `timespec`. x86-64 and AArch64 share clock identifiers 0..11, so this is a query, not a guess. Unknown id → `-EINVAL`; `res == NULL` validates the id only (as the kernel does); unmapped `res` → `-EFAULT` |
| `fadvise64(fd, offset, len, advice)` | Validates the descriptor (`-EBADF`), rejects `advice > POSIX_FADV_NOREUSE` (`-EINVAL`), and forwards to host `posix_fadvise`, whose return value *is* an errno, mapped straight through. For an embedder-adopted in-memory stream there is no host page cache to advise, so it returns 0 — truthful because the call is a hint |

`madvise`(28) and `prctl`(157) stay stubs **on purpose**, and the source says
why: guest memory is backed by host heap pages owned by `GuestAddressSpace`, not
by `mmap`, so `MADV_DONTNEED`/`MADV_FREE` would free nothing and the pages would
not read back as zero — the observable half of the call would be a lie; and
`prctl` addresses process/thread control surface that does not exist in this
execution model. Refusing is better than faking, but the inventory still says
`stubbed` for both rather than claiming them.

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
| `futex`(202) | **stub by design**: value comparison real, relative timeout real (`-ETIMEDOUT`), blocking without a timeout refused | cannot be fully fixed before thread creation exists; a guest that contends gets a clean `-ENOSYS` rather than a false wake-up |
| `madvise` fork-family advice (`DONTFORK`/`WIPEONFORK`) | `-ENOSYS` | a guest that asked for it and then forked would find the mapping still there. The main modes - `MADV_DONTNEED` zeroing, hints - are implemented (§6d) |
| `prctl` beyond the documented subset | `-ENOSYS` (seccomp/MM/capabilities) or `-EINVAL` | a guest that installs a seccomp filter is told it could not be installed rather than being told it was |
| Signal **delivery** | dispositions recorded and reported; nothing is delivered (`LIM-022`) | a guest that installs a handler and waits to be signalled still will not be. Needs guest signal frames and an arch-specific return trampoline |
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
8. **Highest-value next items, in order:** a real `futex` wait queue,
   `inotify_*`/`signalfd`, sockets. **Keep the cross tree green**: `scripts/dev.sh cross-test` runs the
   compat suite at both tiers and fails if tier 1 falls back, so it is the guard
   that stops AArch64 support silently rotting.
