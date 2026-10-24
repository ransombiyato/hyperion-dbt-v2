# TESTING

How to build, test, fuzz and differentially validate Project HYPERION.

---

## 1. Prerequisites

A C++20 compiler and CMake ≥ 3.20. For the full workflow (cross-architecture
validation, formatting, sanitizers) run:

```bash
scripts/bootstrap.sh
```

which installs on Debian/Ubuntu: `build-essential cmake ninja-build clang
clang-format clang-tidy llvm gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
binutils-aarch64-linux-gnu qemu-user qemu-user-static gdb-multiarch`.

It also caches the emulator and cross toolchain under `tools/cache/` (the
restore archives are committed), because the development container does not
persist `/usr` between sessions but the repository does. After a reset,
`scripts/toolcache.sh restore` reinstates the compiler and its host-side binutils
runtime libraries offline; `scripts/toolcache.sh status` shows what is cached.
The benchmark and cross-test scripts use the cached `qemu-aarch64-static`
automatically.

---

## 2. Standard workflow

```bash
scripts/dev.sh bootstrap   # configure build/dev
scripts/dev.sh build       # build
scripts/dev.sh test        # run CTest
```

Or directly:

```bash
cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
```

Presets are also provided: `cmake --preset debug`, `release`, `sanitize`,
`cross-aarch64`.

---

## 3. Test suite layout

| Location | Contents |
| --- | --- |
| `tests/harness/` | dependency-free harness (`HYPERION_TEST`, `HYPERION_CHECK`) |
| `tests/unit/` | unit tests, one file per component |
| `tests/differential/` | interpreter vs. real x86-64 hardware |
| `tests/cli/` | end-to-end CLI tests (`cli_smoke.sh` for the blob path, `elf_test.sh` for the ELF path, `wrapper_test.sh` for the installed `hyperion` command, `registry_cli_test.sh` for the `syscall-registry` contract) |
| `tests/fixtures/` | small committed test inputs (`tiny-static.elf`) |
| `tests/guest/fixtures/` | **C sources** for real x86-64 guest programs, compiled by the test itself |
| `tests/guest/process_test.sh` | guest process regression tests (fork/exec/fd/pipe/vfork exec release/clone child stack + BusyBox pipelines) |
| `tests/guest/syscall_compat_test.sh` | guest-level tests of the syscall compatibility layer: adapted syscalls behave as they do natively, a user registry rule enables a syscall the binary does not implement, removing the rule restores `-ENOSYS`, and a corrupt registry changes nothing and is not overwritten |
| `tests/unit/test_syscall_adapters.cpp` | per-adapter ABI translation (epoll/poll/select/pselect6, timerfd, eventfd, memfd, rusage/tms/itimers, fs metadata, `membarrier`) including the invalid-argument paths |
| `tests/unit/test_syscall_registry.cpp` | path precedence, JSON schema, v1 migration, atomic write + `.bak`, permissions, corrupt/newer-schema handling, user rules surviving an update |
| `tests/unit/test_syscall_dispatch.cpp` | rule compilation and every rejection path, alias argument rewrite/restore, counters, reload snapshot swap |
| `tests/unit/test_json.cpp` | the dependency-free JSON reader/writer, including the malformed inputs the registry has to survive |
| `tests/run_cross_tests.cmake` | cross-arch configure/build/run driver |
| `benchmarks/` | harness smoke tests (`bench.smoke`, `tier_bench.smoke`) |

Registered CTest tests:

| Test | What it proves |
| --- | --- |
| `unit.native` | core components behave correctly on the host |
| `differential.native_vs_interpreter` | the interpreter agrees with real x86-64 |
| `cli.smoke` | `hyperion run` executes a blob through both tiers and they agree |
| `cli.elf` | `hyperion info`/`run` load and execute a static ELF64 image; a non-ELF file still takes the blob path; a truncated ELF is rejected with a clear reason |
| `bench.smoke` | the benchmark harness runs and emits valid output |
| `tier_bench.smoke` | the Tier 0 vs Tier 1 benchmark runs and both tiers are correct (on x86-64 only Tier 0 executes; Tier 1 is reported unavailable) |
| `cross.aarch64_tests` | the suite passes when compiled for AArch64 and run under `qemu-user` |
| `guest.process` | real x86-64 guests create subprocesses correctly: exit status propagates, clone honors `child_stack`, `execve` runs another image and re-execs `/proc/self/exe`, descriptors/pipes survive `fork`, `vfork` unblocks on exec, unsupported clone flags return `ENOSYS`, and an optional static BusyBox runs shell pipelines |
| `guest.process.tier1` | the same suite on the Tier 1 JIT; registered only with `-DHYPERION_ENABLE_GUEST_TIER1_TESTS=ON` (meaningful on an AArch64 host) |
| `guest.syscall_compat` | the compatibility layer as a guest sees it (see below) |
| `cli.syscall_registry` | the `syscall-registry` command's observable contract: resolved path, `0600` creation, `--write` not clobbering, `--validate` never rewriting the file, a rule that shadows a static implementation being refused with an explanation, `--json` agreeing with the effective rule set, exported defaults round-tripping |
| `syscall_bench.smoke` / `syscall_bench.rejects_single_sample` | the dispatch microbenchmark runs, emits plausible numbers, and refuses a single-sample request rather than publishing a spread it cannot compute |

`tests/cli/cli_smoke.sh` writes a small x86-64 blob, runs it through tier 0 and
tier 1, and requires the same status and step count. On a host that cannot
execute generated code, tier 1 falls back to the interpreter (and says so); the
cross pipeline runs the same script with `HYPERION_EMULATOR` set to `qemu-aarch64`
and additionally requires tier 1 to have translated a non-zero number of blocks,
so a vacuous "ok" cannot pass.

`tests/cli/elf_test.sh` runs the same two-tier check over `tests/fixtures/tiny-static.elf`,
a hand-built static `ET_EXEC` whose only instruction is `hlt` at its entry
(`0x401000`): a successful run is exactly one retired instruction with `rip` at
the entry. It also pins the two failure paths a user hits — a file without the
ELF magic must still take the raw-blob path, and a truncated ELF must be rejected
with a clear reason rather than crashing.

The ELF parser handles untrusted input, so `tests/unit/test_elf.cpp` includes
`elf.survives_random_and_mutated_input`: a bounded, deterministic pseudo-fuzz
(2000 random buffers plus byte-mutations of a valid image through parse and map).
It is reproducible from its seed, and under ASan/UBSan any out-of-bounds access
becomes a test failure. A real libFuzzer target can replace it later.

### Guest process tests

`guest.process` is the only suite that executes **real compiled x86-64 guest
programs**. Its fixtures are C sources under `tests/guest/fixtures/`, compiled by
`tests/guest/process_test.sh` with `cc -O2 -static` — the same mechanism
`tests/cli/cli_smoke.sh` and `scripts/run_real_benchmarks.sh` already use for
guest binaries. The script verifies each fixture really is an `EM_X86_64` ELF
before running it, so a host-architecture binary can never be mistaken for a
guest test.

The suite is **only registered when a compiler that can produce a static x86-64
ELF was found**: the host `cc` on x86-64, otherwise `x86_64-linux-gnu-gcc`, or
whatever `HYPERION_GUEST_CC` names. Registering it unconditionally would let a
missing toolchain look like a passing suite.

Every case asserts the guest's **stdout and exit status**, never the translator's
own report. That distinction is the point: a guest printing
`sh: can't fork: Function not implemented` is a failure even when the outer run
reports `status: ok`.

```sh
# Run just the guest tests, with the BusyBox pipeline cases enabled:
cmake -S . -B build -G Ninja -DHYPERION_BUSYBOX=/path/to/busybox-x86_64
cmake --build build
ctest --test-dir build -R guest.process --output-on-failure

# On an AArch64 host (or under qemu), also cover the Tier 1 JIT:
cmake -S . -B build -G Ninja \
  -DHYPERION_BUSYBOX=/path/to/busybox-x86_64 \
  -DHYPERION_ENABLE_GUEST_TIER1_TESTS=ON

# Cross-compiling the fixtures on a non-x86-64 host:
cmake -S . -B build -G Ninja -DHYPERION_GUEST_CC=x86_64-linux-gnu-gcc
```

`HYPERION_BUSYBOX` is optional; when unset the two shell-pipeline cases report
`skip` rather than failing, and the remaining nine process cases still run. A
suitable fixture is Debian's `busybox-static` (`/bin/busybox` from that package
is a statically linked x86-64 ELF). To include the BusyBox pipelines in the
AArch64-under-QEMU cross pipeline, set `HYPERION_BUSYBOX` in the environment
before running `scripts/dev.sh cross-test`.

### The syscall compatibility tests

Three suites cover the compatibility layer, deliberately at different levels,
because each one has failed to catch something the others missed:

- **`unit.native`** (`test_syscall_adapters.cpp`, `test_syscall_registry.cpp`,
  `test_syscall_dispatch.cpp`, `test_json.cpp`) — the compiled logic, including
  every rejection path. Cheap, exhaustive, and it is where a structure offset or
  a flag mapping is pinned.
- **`cli.syscall_registry`** — the command a user actually runs. It exists
  because `--validate` used to exit `0` while silently dropping a rule, and
  `--json` used to print the file's contents instead of the effective rule set:
  both are "works as designed, misleads the user" defects that no unit test on
  the compiler would have caught.
- **`guest.syscall_compat`** — a real statically linked x86-64 program
  (`tests/guest/fixtures/compat_syscalls.c`) doing 27 behavioural checks: epoll
  round-trip with a 64-bit payload tag, poll/select readiness, timerfd, memfd,
  `getrusage`, `times`, limits, `umask`, `mkdtemp`, truncate/fsync, symlink and
  hard link, chdir/getcwd, sched_yield, membarrier, and a 60 ms `nanosleep` whose
  elapsed time is measured rather than trusted. The same binary is run natively
  first and must pass, so the test cannot be wrong about what correct behaviour
  is.

The guest suite also proves the registry *causes* the behaviour rather than merely
accompanying it: the fixture uses syscall `300`, which does not exist on x86-64,
and the test writes a rule mapping it to `getpid(2)`. It succeeds with the rule,
returns `-ENOSYS` without it, and the corrupt-registry case proves a broken file
neither changes behaviour nor gets overwritten.

Two lessons worth keeping:

- A stub is worse than a refusal, and only a guest can tell the difference. The
  `nanosleep` stub returned success instantly; the fixture catches it by measuring
  elapsed time, which no exit-status check ever would.
- Run the sanitizer build. The `poll` adapter read eight bytes out of the
  sixteen-bit `events` field of an eight-byte `struct pollfd`. The overrun stayed
  inside the same stack frame, so every ordinary build passed; ASan did not.

### Diagnosing a guest that misbehaves

`HYPERION_TRACE_SYSCALLS=1` logs every guest system call with its result, tagged
with the host pid so parent and child are distinguishable:

```sh
HYPERION_TRACE_SYSCALLS=1 ./build/hyperion run busybox-x86_64 sh -c 'true' 2>trace.txt
grep ' -> unimplemented' trace.txt     # what the guest asked for that we lack
```

The trace is written with `write(2)` rather than stdio so it cannot be swallowed
by, or corrupt, the guest's own buffered output.

### Writing a test

```cpp
#include "harness/test_harness.hpp"

HYPERION_TEST("component", "behaviour") {
    HYPERION_CHECK_EQ(actual, expected);
}
```

Test binaries accept `--filter=substring` and `--list`.

---

### The Android syscall-policy tests

`tests/unit/test_host_syscalls.cpp` (suite `host_syscalls`) covers the
`SIGSYS`-avoidance described in `COMPATIBILITY.md` §5.07. It does not merely
assert that a branch was taken: it installs a real `SECCOMP_RET_TRAP` filter -
the action Android's app policy uses - on a forked child, and requires both

* the native path to die from `SIGSYS` with the trap installed (the Android 10
  failure, reproduced on an ordinary Linux host), and
* the portable path to answer the guest's `statx` correctly with the same trap
  installed.

If HYPERION ever goes back to issuing `statx(2)` unconditionally, the second half
fails. The rest of the suite also runs in the portable mode in CI:

```bash
HYPERION_PORTABLE_SYSCALLS=1 ctest --test-dir build --output-on-failure -E '^cli\.install_sigsys$'
```

Run the whole suite in that mode locally too after touching
`src/core/syscall.cpp` or `src/core/syscall_adapters.cpp`.

### Platform skips

A test whose precondition genuinely cannot be met calls `HYPERION_SKIP(reason)`.
The harness reports it as `[ SKIP ] name (reason)`, counts it separately, and
never counts it as a pass. Skips in use today:

| Test | Skipped when | Why it is a platform limit, not a bug |
| --- | --- | --- |
| `host_syscalls.native_linux_reports_statx_and_memfd_usable`, `host_syscalls.an_android_api_level_below_30_disables_both` | the binary is built for Android | the API level then comes from the device (`ro.build.version.sdk`); the child-process cases exercise the same decision function everywhere |
| `host_syscalls.trapped_statx_kills_the_native_path_and_not_the_portable_one` | the host refuses `PR_SET_SECCOMP` (seen under `qemu-user`) | the reproduction needs a filter that can be installed; the portable alternative is still covered by the `HYPERION_PORTABLE_SYSCALLS` cases |
| `translator.*`, `exec_mem.*`, `translation_cache.*` AArch64 cases (24 in total on x86-64) | the host is not AArch64 | the generated code *is* AArch64; the cross build runs them under `qemu-user` |
| `interpreter.run_program_matches_hardware_for_a_loop` | the host is not x86-64 | the reference is real hardware execution of the same bytes |
| `translation_cache.refuses_to_run_without_exec_support` | the host *can* execute generated code | it asserts the refusal path of a host that cannot |
| `differential.*` | the host is not x86-64 | same reason as the hardware differential |

None of these is a "the feature is broken" skip, and each one names its reason in
the test output. `HYPERION_SKIP` throws, so a skipped test can never fall through
into its assertions.

### Temporary files

Tests never hardcode `/tmp`: Termux on Android has no `/tmp` at all, and its only
writable temporary directory is `$TMPDIR`. `tests/harness/test_tempdir.hpp`
provides `TempDir` (a private directory under the first usable base of
`$TMPDIR`, `/tmp`, `$HOME/.cache`, `$PREFIX/tmp`, the current directory,
removed recursively afterwards), plus `ScopedCwd` and `ScopedUmask` so a failed
assertion cannot leave the rest of the binary in a changed working directory or
with a changed file-mode mask. The shell tests do the same with `mktemp -d` and
point `XDG_DATA_HOME`/`HOME` at their own tree so they neither read nor write the
user's real syscall registry.

## 4. Cross-architecture validation

The reference target is AArch64; development hosts are often x86-64. Generated
AArch64 code cannot run natively on x86-64, so validation is done by
cross-compiling and executing under `qemu-user`:

```bash
scripts/dev.sh cross-test
```

This configures `build/cross-test/cross-aarch64` with
`cmake/toolchains/aarch64-linux-gnu.cmake` (static linking), builds the test
suite, and runs the unit, differential, CLI, and real x86-64 guest-process tests
under `qemu-aarch64`. The guest-process suite runs at both Tier 0 and Tier 1;
`HYPERION_BUSYBOX` optionally enables the two shell-pipeline cases. Any failure
fails the CTest run.

The same driver also runs, against the **AArch64-built** binary at both tiers:

- `tests/guest/syscall_compat_test.sh` — the whole compatibility suite, so the
  adapters are exercised while the guest executes JIT-generated AArch64 code, not
  just from the interpreter;
- an explicit tier-1 assertion: the run must print `tier 1: N basic block(s)
  translated` with `N > 0` **and** must not print a fallback warning. Without
  that, a JIT that silently fell back to the interpreter would pass every
  behavioural check and prove nothing;
- `tests/cli/registry_cli_test.sh` — path resolution and permissions on the
  AArch64 build.

Re-measured 2026-10-24 (aarch64-linux-gnu-g++ 14.2.0, `qemu-aarch64-static`
from `tools/cache`). The unit and differential rows were re-run at the end of the
Android-syscall work; the guest-process, compat and CLI rows are from the same
toolchain and were not re-run afterwards:

| Run | Result |
| --- | --- |
| AArch64 unit tests | **360 passed, 0 failed, 3 skipped** (of 363 registered) |
| AArch64 differential suite | 0 passed, 0 failed, 11 skipped (correct: no x86-64 oracle on AArch64, every case SKIPs) |
| AArch64 guest process, tier 0 / tier 1 | passed / passed |
| AArch64 syscall compat, tier 0 / tier 1 | 11/11 checks, 43 guest assertions each |
| AArch64 tier-1 JIT evidence | `tier 1: 1412 basic block(s) translated`, `failures 0`, 170 308 guest steps — identical to tier 0 |
| AArch64 registry CLI | 11/11 checks |

To reproduce the toolchain on a fresh Debian container without root-level
guessing: `sudo apt-get install -y --no-install-recommends g++-aarch64-linux-gnu`,
then `scripts/dev.sh cross-test` (it prefers the repo-cached
`tools/cache/bin/qemu-aarch64-static`).

**Limits.** `qemu-user` validates *correctness*, never performance, and it is
not a cycle-accurate model. Performance claims require real AArch64 hardware.
AArch64-hosted runs (`unit.native` on an AArch64 machine) additionally verify
the pipeline without qemu.

**Translator differential.** `tests/unit/test_translator.cpp` uses this pipeline
directly. Its scope tests (which instructions are accepted and which are refused)
run on any host; its differential tests execute the *generated AArch64* and
compare against the reference interpreter. Those differential cases compile only
under `__aarch64__` and report `SKIP` on x86-64 rather than a false pass. There
are two levels:

- **Single-instruction** — one instruction, every register and all six flags
  compared; plus an 847-case operand sweep over carry/overflow/parity boundaries.
- **Whole-program** — a counted loop and taken/not-taken branches run through
  `TranslationCache` and compared against `run_program` on final `GuestState`
  and step count. `translator.dispatcher_*` additionally proves an unsupported
  instruction stops the run (leaving `rip` on it) and an infinite loop is
  reported as a step limit rather than hanging.

`tests/unit/test_translation_cache.cpp` covers the cache directly: it must
refuse to run when the host cannot execute generated code, and on AArch64 it
must cache blocks (not re-translate each visit) and agree with the interpreter.

---

### The installer's SIGSYS decision

`tests/cli/install_sigsys_test.sh` (CTest target `cli.install_sigsys`) drives real
`install.sh` runs against a throwaway CMake project whose only tests are
`unit.native` and `other.ok`, with `unit.native` killed by `SIGSYS`. The prompt is
exercised through a pseudo-terminal (`script -qec`), which is the only way to
observe what a person would actually see, and every case is checked against the
installer's real behaviour:

| Scenario | Required outcome |
| --- | --- |
| only `unit.native` failed, reason `SIGSYS`, answer `yes` | install succeeds; output says the test was **skipped, not passed**; `install.conf` records `HYPERION_SKIPPED_TESTS=unit.native` |
| same, bare Enter / EOF / no terminal | default **no**: the test stays enabled and the install aborts; nothing is installed |
| same, `--skip-sigsys-test` or `HYPERION_SKIP_SIGSYS_TEST=1` | no question, skip applied, still reported as skipped |
| `SIGSYS` **and** another failing test | aborts immediately; the question is never asked (skipping cannot hide an unrelated failure) |
| `unit.native` fails with output that merely *mentions* SIGSYS | aborts: only ctest's own `(SIGSYS)` verdict counts |
| healthy suite with `--skip-sigsys-test` | everything runs, "all tests passed", no skip recorded |

## 5. Sanitizers

```bash
scripts/dev.sh sanitize
```

Builds with AddressSanitizer + UndefinedBehaviorSanitizer and runs the suite.
The translator manipulates raw memory and generates executable code, so
sanitizers are expected to be central to correctness work from Phase 2 onward.
Note: ASan interacts with custom code caches; where it must be disabled for a
specific test, document why in the test.

Measured on 2026-10-24 (GCC 14.2.0, Debian 13):

```
cmake -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DHYPERION_ENABLE_SANITIZERS=ON
cmake --build build/asan -j8
ctest --test-dir build/asan        # 17/17 passed
```

The first run of this build failed immediately, in the `poll` adapter: an eight
byte read from an eight-byte `struct pollfd` buffer through a pointer four bytes
in (`get_u64_le(raw + 4)`). The overflow landed inside the same stack frame, which
is why no ordinary build ever noticed. It is worth running this build before
believing any structure-layout code in the adapters.

---

## 5.1 Dispatch microbenchmark

`benchmarks/syscall_bench.cpp` (target `hyperion_syscall_bench`, CTest
`syscall_bench.smoke`) measures the cost of each dispatch layer, so a change to
the hot path can be argued with a number instead of an opinion:

```sh
./build/dev/benchmarks/hyperion_syscall_bench --iterations 300000 --repeats 9
./build/dev/benchmarks/hyperion_syscall_bench --json          # machine-readable
```

Method: one warm-up sample is discarded, the median and interquartile range of
the remaining samples are reported, an optimisation barrier stops the loop being
deleted, and every sample's checksum is compared so a miscompiled loop cannot
look fast. A plausibility floor (BENCH-001) rejects results below the cost of a
bare host syscall, and `--repeats 1` is refused outright.

Measured 2026-10-24, x86-64 Debian 13, GCC 14.2.0 `-O2`, 300 000 iterations per
sample, 9 samples (1 discarded):

| Target | median ns | p25 | p75 | what it measures |
| --- | --- | --- | --- | --- |
| `host_getpid` | 121.0 | 120.7 | 121.7 | bare host `getpid(2)`, the floor for any handler |
| `host_sched_yield` | 221.6 | 221.2 | 222.6 | bare host `sched_yield(2)`, the operation the adapter performs |
| `direct_adapter` | 224.1 | 223.8 | 224.4 | the compiled adapter called directly, without dispatch |
| `adapter_dispatch` | 237.4 | 236.8 | 237.8 | `handle()` → rule table → adapter (layer 1) |
| `existing_dispatch` | 133.7 | 133.3 | 133.9 | `handle()` → static switch (layer 2) |
| `unsupported` | 12.9 | 12.8 | 13.0 | `handle()` → `-ENOSYS` + counting (layer 4) |

The rule table's own overhead (layer 1 minus a direct call) is **≈13 ns per
call**, which is smaller than the noise of the syscall it dispatches to
(`sched_yield` is ~222 ns). The refusal path is the cheapest of all, so an
unsupported syscall cannot become a hot spot — but note that it is cheap because
it does no I/O: it counts, and the reporting happens once, at the end of the run.

**What these numbers are not.** They are host-CPU measurements of the dispatch
layer only. They exclude guest instruction translation, and they say nothing about
an AArch64 device — this host is x86-64 and could not run Tier 1 at all. Do not
quote them as end-to-end performance.

---

## 6. Clang build (matches CI)

```bash
scripts/dev.sh clang-build
```

CI builds every target twice, once with gcc and once with clang, both with
`HYPERION_WARNINGS_AS_ERRORS=ON`. Clang's `-Wsign-conversion` (enabled by
`-Wconversion`) is stricter than gcc's, so a change that builds cleanly with gcc
can still fail CI. Run this before pushing a change that touches integer
conversions or container indexing.

---

## 7. Formatting and static analysis

```bash
scripts/dev.sh format       # check (fails on drift)
scripts/dev.sh format-fix   # apply
clang-tidy -p build/dev src/core/*.cpp   # static analysis (config: .clang-tidy)
```

CI enforces `clang-format --dry-run --Werror` on all sources, using **pinned
clang-format 19.1.7** (see the `format` job in `.github/workflows/ci.yml`).
Formatting rules change between releases, so use the pinned version locally to
avoid spurious drift.

### Warning flags

The warning set in `CMakeLists.txt` includes **`-Wsign-conversion`** in addition
to `-Wconversion`. This matters: clang turns `-Wsign-conversion` on as part of
`-Wconversion`, but gcc does not, so a gcc-only host would silently miss a
signed index reaching `vector::operator[]` that the clang CI job rejects. With
the flag enabled on both compilers, `scripts/dev.sh build` alone is enough to
catch that class of bug — clang is no longer required locally to reproduce it.

---

## 8. Differential testing (Phase 2+)

The core correctness strategy:

1. **Reference interpreter** executes the same IR as the code generator; it is
   the trusted oracle.
2. **Native x86-64 oracle**: the same test program is compiled natively for
   x86-64 and executed on the host; its observable output (registers, flags,
   memory, exit code, stdout) is compared with the translated run.
3. **QEMU oracle**: guest binaries may also be run under `qemu-x86_64` for an
   independent third opinion.
4. **Randomized differential testing**: generate random instruction sequences
   with a known-good assembler, execute under both the oracle and the
   translator, compare full guest state after each instruction.

A test that only checks a subset of state is not a differential test. Record
which state fields are compared.

### Verifying AArch64 encodings against binutils

The AArch64 encoder's tests use expected words taken from GNU `as`/`objdump`,
not values written from memory. To add or re-check an encoding, assemble the
instruction and read the word out of the disassembly:

```sh
printf '.text\nadd x3, x4, x5\n' > /tmp/ref.s
aarch64-linux-gnu-as -o /tmp/ref.o /tmp/ref.s
aarch64-linux-gnu-objdump -d /tmp/ref.o | tail -n +8
#   0:   8b050083        add     x3, x4, x5
```

Paste the hex word into the test with a comment naming the instruction it came
from. If the encoder and the test disagree, one of them is wrong — find out
which before changing either. Never "fix" a test by copying the encoder's
output; that makes the test tautological and worthless.

---

## 9. Fuzzing (Phase 2+)

- **Decoder fuzzing**: feed random byte streams to the decoder; it must never
  crash, never read out of bounds, and must reject invalid encodings. Suitable
  for libFuzzer.
- **Semantics fuzzing**: random operand values against the oracle.
- **Differential block fuzzing**: random valid basic blocks, compared
  instruction-by-instruction.

Fuzz targets live in `tests/fuzz/` (created in Phase 2) and run in CI on a
time budget, with longer runs on demand.

---

## 10. CI

`.github/workflows/ci.yml` runs on every push and pull request:

| Job | Purpose |
| --- | --- |
| `build & test (gcc)` / `build & test (clang)` | compile clean with `-Werror` and run the native suite; clang is stricter (`-Wsign-conversion`) |
| `AArch64 cross tests (qemu-user)` | AArch64 cross-build + tests under `qemu-user` |
| `clang-format` | `clang-format --dry-run --Werror` (pinned 19.1.7) |
| `ASan/UBSan` | clang + Address/UndefinedBehavior sanitizer test run |
| `syscall inventory & shellcheck` | inventory validator + shellcheck on scripts |

A red CI job blocks merging. Never mark a milestone complete while CI is failing.
Run `scripts/dev.sh clang-build` locally before pushing: a gcc-clean tree can
still fail the clang job.

`.github/workflows/release.yml` runs on a `v*` tag (or manually): it builds,
runs the full test suite, packages both architectures with `scripts/package.sh`,
smoke-tests the packaged binaries (the AArch64 one under `qemu-user`, with a real
`--tier=1` translation whose result is checked), verifies `SHA256SUMS`, and
publishes a GitHub pre-release. A release is never published from a build that
fails its own tests.

---

## 11. Release packaging (local)

```bash
scripts/package.sh              # dist/hyperion-<v>-{x86_64,aarch64}-linux.tar.gz + SHA256SUMS
scripts/package.sh --no-cross   # x86-64 only (no cross compiler required)
hyperion package                # same, via the wrapper
```

To verify a tarball by hand, exactly as the release workflow does:

```bash
tar -xzf dist/hyperion-<v>-x86_64-linux.tar.gz -C dist
dist/hyperion-<v>-x86_64-linux/hyperion version          # runs natively
tar -xzf dist/hyperion-<v>-aarch64-linux.tar.gz -C dist
qemu-aarch64 dist/hyperion-<v>-aarch64-linux/hyperion version
printf '\x48\xb8\x01\x00\x00\x00\x00\x00\x00\x00\x48\x83\xc0\x02\xf4' > /tmp/t.bin
qemu-aarch64 dist/hyperion-<v>-aarch64-linux/hyperion run /tmp/t.bin --tier=1 --dump
# must report: 1 basic block(s) translated / status: ok / rax =0x0000000000000003
(cd dist && sha256sum -c SHA256SUMS)
```

The last check is the one that matters: it proves the shipped AArch64 binary
really translates and executes generated code rather than being a stub. It is
*not* a performance measurement (`qemu-user` is not the target hardware).
