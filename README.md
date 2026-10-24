# Project HYPERION

**A high-performance x86-64 → AArch64 user-mode dynamic binary translator (`Box64`-class ELF64 + dynamic linker + ARMv8 NEON/FP JIT).**

HYPERION translates and executes real Linux x86-64 user-space binaries — both **statically linked** (`ET_EXEC` / static-PIE `ET_DYN`) and **dynamically linked** (`PT_INTERP` `/lib64/ld-linux-x86-64.so.2`, shared `.so` libraries loaded via guest `mmap` + `MAP_FIXED`, `libc.so.6`, `libm.so.6`, `libstdc++.so.6`, `libgcc_s.so.1`) — directly on AArch64 and x86-64 Linux hosts with zero root privileges and zero kernel modules. *(Standalone full-system x86-64 PC hardware + generic `.iso` OS booting lives in [`ransombiyato/gallyx`](https://github.com/ransombiyato/gallyx).)*

The reference target device is a **Qualcomm Snapdragon 855 running ARM64
Android 10, 8 GB RAM, no root and no KVM**. Development and testing also happen
on other Linux hosts, so target-device compatibility is *measured, not
assumed* — see [`docs/target-environment.md`](docs/target-environment.md).

> **Status: Phases 0–7 complete on single-threaded static x86-64 Linux ELF64
> binaries, including programs that create subprocesses.** The repository builds clean with **zero warnings** across GCC and
> Clang (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
> -Werror`), passes ASan + UBSan, `clang-tidy`, `shellcheck`, and full AArch64
> cross-execution tests, and executes **unmodified `gcc 14.2 -O2 -static`
> (`glibc 2.41`) Linux C binaries end-to-end** across **two full execution
> tiers**:
> - **Tier 0 (Fast Reference Interpreter):** Full baseline x86-64 + 128-bit SSE2
>   ISA, 512-entry direct-mapped `SoftTlb`, and 4096-entry decoded instruction
>   cache (**56–61 MIPS** single-core x86-64 host throughput).
> - **Tier 1 (Full-System AArch64 + 128-bit ARMv8 NEON JIT):** Shared trampoline
>   ABI, 4096-slot in-JIT direct jump table, inline `SoftTlb` load/store fast
>   path, 5-register block allocation, branchless `CSEL` exits, native self-loop
>   superblocks, and hardware 128-bit ARMv8 NEON SIMD lowering — achieving
>   **104.5× speedup** on `hyperion_tier_bench` (`2.54 ns/insn`) and **5.17× to
>   20.12× end-to-end speedup (10.43× overall)** across `120,313,691` dynamic
>   instructions in 5 real `glibc 2.41` C binaries with **zero step discrepancy**
>   against Tier 0. See [`docs/real-world-evaluation.md`](docs/real-world-evaluation.md).
>
> **Process support (2026-10-23).** `fork`, `vfork`, the process-creating subset
> of `clone` (including `child_stack`), `execve` and `wait4` are implemented, so
> shell pipelines and programs that spawn children run. Regression coverage
> includes a vfork child that re-execs `/proc/self/exe` and must unblock its
> parent before the new image exits. Verified against a real statically linked
> x86-64 BusyBox on x86-64 and on AArch64 under `qemu-user`, at both tiers:
>
> ```sh
> hyperion run --quiet busybox-x86_64 sh -c 'printf "9\n2\n5\n1\n" | sort -n'   # -> 1 2 5 9
> hyperion run --quiet busybox-x86_64 sh -c 'echo benchmark | wc -c'              # -> 10
> ```
>
> Both match the native output byte for byte. **Thread creation is deliberately
> not supported**: `clone` with `CLONE_VM`/`CLONE_THREAD`/`CLONE_FILES`/`CLONE_FS`/
> `CLONE_SIGHAND` returns `-ENOSYS` rather than being silently downgraded to a
> fork, because a guest that asked for a shared address space and quietly received
> a private copy would corrupt its own memory. Multi-threaded guests do not run.
> Namespace-creating flags also return `-ENOSYS` (they need `CAP_SYS_ADMIN`, which
> unrooted Android does not grant), and guest-installed signal handlers are
> recorded but not invoked. See [`COMPATIBILITY.md`](COMPATIBILITY.md) §5.05 for
> the exact matrix and [`TESTING.md`](TESTING.md) to reproduce it
> (`ctest -R guest.process`).
>
> **Scope note (2026-10-19).** Kernel/system-mode emulation is no longer ruled
> out. A measured experiment showed a real Linux kernel running unprivileged
> with `systemd` as PID 1, and an AArch64 port of that mechanism exists. It is
> under investigation, not implemented — see
> [`docs/architecture-feasibility.md`](docs/architecture-feasibility.md).

---

## If you are a new agent: start here

Everything needed to continue the project is **in this repository**. In order:

1. Read [`STATUS.md`](STATUS.md) §4 — the single best next action — then
   [`docs/agent-handoff.md`](docs/agent-handoff.md) (where we are, how to verify,
   what to do next) and [`ROADMAP.md`](ROADMAP.md).
2. Restore the toolchain and build. `cmake`/`ninja`/`shellcheck` come from
   committed `.deb`s and the AArch64 cross toolchain comes from the committed
   tarball, so this works from a **fresh clone with no package mirror**:

   ```bash
   scripts/toolcache.sh install-host   # cmake/ninja/shellcheck from committed debs
   scripts/toolcache.sh restore        # AArch64 cross toolchain from the committed tarball
   scripts/dev.sh build && scripts/dev.sh test    # expect: all tests passed
   scripts/dev.sh cross-test           # AArch64 under qemu-user
   ```

3. Work in small, verified increments and commit each one. Correctness before
   speed; never claim a test passed or a push succeeded unless it did. The rules
   are in [`docs/agent-handoff.md`](docs/agent-handoff.md) §9.

---

## What is actually implemented today

| Capability | State |
| --- | --- |
| Build system (CMake, C++20) | Implemented, builds clean with `-Werror` |
| Unit tests + CTest integration | Implemented, passing |
| AArch64 cross-build + qemu-user validation pipeline | Implemented, passing |
| Host capability probe (arch, page size, cache line, W^X) | Implemented |
| Host benchmark harness (JSON, repeated-run statistics) | Implemented (host-only) |
| One-line installer + `hyperion` command | Implemented (`info <exe>`, `run <exe>`, `status`, `doctor`, `build`, `test`, `bench`, `cross-test`) |
| Static ELF64 x86-64 loader + SysV stack | **Implemented** (`ET_EXEC` + static-PIE `ET_DYN`, `PT_LOAD` W^X + BSS zero-fill, `argc`/`argv`/`envp`/`auxv`) |
| x86-64 decoder (baseline psABI + SSE2) | **Implemented** — 8/16/32/64-bit integer + shifts/rotates + mul/div + `SETcc`/`CMOVcc`/`BSWAP`/`BSF`/`BSR`/`BT*`/atomics + `FS:` TLS + `REP` string ops + 40+ 128-bit SSE2 ops (`27,835 / 27,835` unique `glibc 2.41` instructions verified) |
| Guest-state model + reference interpreter | **Implemented** — 512-entry `SoftTlb` + 4096-slot decoded ICache (**56–61 MIPS** on x86-64 host; bit-exact across `120.3M` dynamic `glibc` instructions) |
| AArch64 encoder (binutils-verified ARMv8 + NEON) | **Implemented** — scalar GPR loads/stores, shifts/rotates, bitfields, `mul`/`smulh`/`umulh`, `csel`, `rev`/`rbit`/`clz`, `adr`, `br`/`blr`, and 128-bit NEON SIMD |
| W^X executable memory (`ExecMem` + `ExecArena`) | **Implemented** — single-block `ExecMem` plus 16-byte-aligned multi-page W^X `ExecArena` |
| x86-64 → AArch64 instruction translation | **Implemented** — standalone `translate_block()` plus full-system `translate_cached_block()` covering memory, stack (`PUSH`/`POP`/`CALL`/`RET`/`LEAVE`), full integer ALU, and hardware 128-bit ARMv8 NEON SIMD |
| Multi-instruction program translation + code cache | **Implemented** — shared trampoline ABI, in-JIT 4096-slot jump table, branchless `CSEL` exits, self-loop superblocks, inline `SoftTlb` fast path (**104.5×** microbench / **5.17×–20.12×** full `glibc` binary speedup) |
| Linux syscall layer | **Partial** — **128 of 375** numbers answered (122 implemented, 6 stubbed), CI-checked against `docs/syscalls/inventory.tsv`; `-ENOSYS` otherwise. Counts and the full list are in `STATUS.md` |
| Syscall compatibility engine (adapters + persistent registry) | **Implemented** — 4-layer dispatch (compiled rule table → static switch → legacy compat switch → `-ENOSYS`), 39 ABI adapters, and a validated external JSON registry at `~/.local/share/hyperion/syscalls/registry.json`, created on install and never overwritten by an upgrade. See `ARCHITECTURE.md` §6 |
| Syscall observability | **Implemented** — per-layer counters, unique-vs-total unsupported accounting, `HYPERION_TRACE_SYSCALLS=1`, and `hyperion syscall-registry --list/--validate/--json` |
| AArch64 / Tier 1 validation of the syscall layer | **Verified** — cross tree 10/10; the compat guest runs on JIT-generated AArch64 code with 1412 translated blocks and the same step count as the interpreter (numeric details in `docs/syscall-engine-report.md` §6a) |
| Guest address space (mmap/brk/NX) | **Implemented** (Phase 6) — page-granular `split_region_at()` `mprotect`/`munmap`, enforced `MemPerm::kExec` fetch permissions, reserved heap window, per-mapping size cap (D-029) |
| Guest descriptor table (unbuffered POSIX `OpenFileDescription`) | **Implemented** (Phase 6) — refcounted `OpenFileDescription` supporting `dup`/`dup2`/`dup3`/`fcntl`, positional `pread64`/`pwrite64`, scatter/gather `readv`/`writev`, and `adopt()` |
| Register allocation | **Implemented within a block** (Phase 5, D-021) — hottest guest registers kept in `x19..x23` across block bodies, memory ops, SSE2 loops, and self-loop superblocks |
| Block chaining & cache invalidation | **Implemented** — direct in-JIT 4096-slot jump table + native self-loop superblocks + `Memory::code_epoch()` invalidation exclusively on executable `kExec` changes |

The syscall compatibility engine (dispatch layers, ABI adapters, registry schema,
measurements, remaining gaps and a handoff note) is described in
[`docs/syscall-engine-report.md`](docs/syscall-engine-report.md).

See [`docs/real-world-evaluation.md`](docs/real-world-evaluation.md) for the complete
real-binary compatibility & multi-sample performance evaluation across five compiled
`gcc 14.2 -O2 -static` (`glibc 2.41`) C workloads (`120,313,691` dynamic instructions).

This table is the single source of truth for "what works". It is updated at
every milestone. If a capability is not listed as implemented, assume it does
not exist.

---

## Repository map

```
CMakeLists.txt          Top-level build definition
CMakePresets.json       Standard configure/build/test presets
cmake/toolchains/       AArch64 cross-compilation toolchain file
include/hyperion/       Public headers
src/core/               Core library (host probe, decoder, interpreter,
                        AArch64 encoder, executable memory, translator)
src/cli/                Command-line entry point
tests/harness/          Dependency-free C++ test harness
tests/unit/             Unit tests
tests/run_cross_tests.cmake  Cross-arch validation driver
tests/bench_rejects_bad_args.cmake  Negative test for the benchmark harness
benchmarks/             Benchmark harness
experiments/            Feasibility experiments (NOT part of the translator)
scripts/                Developer task runner, env bootstrap, `hyperion` CLI
install.sh              One-line installer (curl | bash)
docs/                   Agent handoff, known issues, target environment
.github/workflows/      CI
```

Documentation index:

| File | Purpose |
| --- | --- |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | System design and component boundaries |
| [`ROADMAP.md`](ROADMAP.md) | All phases, milestones and completion status |
| [`STATUS.md`](STATUS.md) | Current state, last successful test, next task |
| [`DECISIONS.md`](DECISIONS.md) | Architectural decisions and reasoning |
| [`COMPATIBILITY.md`](COMPATIBILITY.md) | Implemented vs missing instruction/ABI features |
| [`docs/syscalls/README.md`](docs/syscalls/README.md) | Syscall inventory (U1–U4, E1–E3): tiers, evidence and validation |
| [`BENCHMARKS.md`](BENCHMARKS.md) | Benchmark methodology and measured results |
| [`TESTING.md`](TESTING.md) | Build, test, fuzzing and differential-testing guide |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Coding conventions and workflow |
| [`SECURITY.md`](SECURITY.md) | Threat model and safe execution boundaries |
| [`CHANGELOG.md`](CHANGELOG.md) | Meaningful changes by milestone |
| [`THIRD_PARTY.md`](THIRD_PARTY.md) | Dependency and licence inventory |
| [`docs/agent-handoff.md`](docs/agent-handoff.md) | Instructions for a new agent |
| [`docs/known-issues.md`](docs/known-issues.md) | Bugs, limitations and blocked work |
| [`docs/target-environment.md`](docs/target-environment.md) | Reference device and host matrix |
| [`docs/glossary.md`](docs/glossary.md) | Terminology used in this project |
| [`docs/architecture-feasibility.md`](docs/architecture-feasibility.md) | Scope analysis: user-mode vs system-mode vs hybrid, with measured evidence |
| [`docs/real-world-evaluation.md`](docs/real-world-evaluation.md) | End-to-end usability & multi-sample performance evaluation on real static `glibc 2.41` binaries |

---

## Quick start

### One-line install

```bash
curl -fsSL https://raw.githubusercontent.com/ransombiyato/hyperion-dbt-v2/main/install.sh | bash
```

This needs no root, writes nothing outside `~/.local` (override with
`--prefix`), builds and tests the project, and installs two commands:
`hyperion` (the full front end) and `hypy` (the short launcher for running a
guest). Run from inside a checkout it builds that checkout instead of
re-downloading it, which matters on a device with no network.

```bash
hypy <elf> [args...]   # run a program; exit status = the guest's own
hyperion probe         # report host capabilities
hyperion doctor        # check the host, toolchain and install
hyperion status        # what is implemented, and what is not
hyperion test          # run the test suite
hyperion help          # every command
```

If you would rather read a script before running it — always reasonable — the
installer is a plain, commented shell script:

```bash
curl -fsSLO https://raw.githubusercontent.com/ransombiyato/hyperion-dbt-v2/main/install.sh
less install.sh && bash install.sh
```

To remove it again: `bash install.sh --uninstall --prefix ~/.local`.

### Running a program

```bash
# A static x86-64 BusyBox running a real shell pipeline, under translation:
hypy "$HOME/hyperion-test/busybox-x86_64" sh -c 'printf "9\n2\n5\n1\n" | sort -n'
# 1
# 2
# 5
# 9

hypy --quiet "$HOME/hyperion-test/busybox-x86_64" sh -c 'echo benchmark | wc -c'
# 10
```

Everything after the program path is passed to the guest unchanged, so guest
options that start with `-` need no escaping. `--tier=0` forces the reference
interpreter and `--tier=1` the AArch64 JIT; on an x86-64 host, where generated
AArch64 code cannot execute, hyperion says so on stderr and falls back to the
interpreter. `--quiet` suppresses the banner so only the guest's output and exit
status remain, which is what you want in a script or CI.

> **Both output and exit status are the guest's.** A guest that prints
> `sh: can't fork: Function not implemented` and exits 1 is a *failure*, and
> hyperion now exits 1 too — it used to exit 0 unless `--quiet` was given, which
> quietly told any exit-status gate that a broken run had succeeded.
>
> `hyperion bench` measures the host machine, not emulation; `hyperion_tier_bench`
> measures Tier 0 vs Tier 1 guest execution. See `BENCHMARKS.md`.

### The syscall compatibility registry

Some syscalls cannot be handled by copying arguments: the guest and host disagree
about a structure layout or a flag value, so they need real translation. HYPERION
ships those translations as **adapters** (C++ code compiled into the binary) and
lets a **JSON registry outside the install directory** decide which syscall
numbers they serve. The registry is configuration only: it can name a compiled-in
adapter, never a host syscall number, a library or a command.

```sh
hyperion syscall-registry --list                  # what is active, and from where
hyperion syscall-registry --validate              # check the file (never modifies it)
hyperion syscall-registry --json                  # machine-readable summary
hyperion syscall-registry --export-defaults       # the shipped rules
hyperion run --no-syscall-registry ./prog         # ignore the file entirely
```

The default path is `$XDG_DATA_HOME/hyperion/syscalls/registry.json`, else
`$HOME/.local/share/hyperion/syscalls/registry.json` (the same on Termux).
`$HYPERION_SYSCALL_REGISTRY` and `--syscall-registry=FILE` override it. Writes
are atomic and keep a `.bak`; a missing, corrupt or newer-schema file never stops
a program from running — the built-in rules stay in force and the problem is
reported. `--validate` exits `3` when the file parsed but a rule was rejected, so
"my rule is not working" is never reported as success.

A rule is a small object:

```json
{
  "id": "user.custom300",
  "name": "custom300",
  "syscall": 300,
  "implementation": "guest_alias",
  "target_syscall": 39,
  "origin": "user",
  "rule_version": 1,
  "enabled": true
}
```

...which makes syscall `300` behave as `getpid(2)`. `tests/guest/syscall_compat_test.sh`
uses exactly this rule to prove that a user rule enables a syscall the binary has
never heard of, and that deleting the rule restores `-ENOSYS`.

### Build from source

Requires a C++20 compiler and CMake ≥ 3.20.

```bash
# Install the full toolchain (Debian/Ubuntu; needs sudo once).
scripts/bootstrap.sh

# Configure, build, test.
scripts/dev.sh bootstrap
scripts/dev.sh build
scripts/dev.sh test

# Inspect the build and the host it is running on.
./build/dev/hyperion version
./build/dev/hyperion probe
```

### Cross-architecture validation

Because the reference target is AArch64 but development often happens on
x86-64, HYPERION validates AArch64 artifacts by cross-compiling and executing
them under `qemu-user`:

```bash
scripts/dev.sh cross-test
```

This configures a second build tree with `aarch64-linux-gnu-g++`, runs the same
test suite inside `qemu-aarch64`, and requires it to pass. Phase 3 already uses
this exact mechanism to validate generated AArch64 code against the x86-64
reference interpreter: `translator.differential_*` executes translated blocks
under `qemu-user` and compares every register and flag against the interpreter.

The development container does not persist `/usr` between sessions, but the
repository does, so the emulator and cross toolchain are cached under
`tools/cache/`; the restore artefacts (the cross toolchain, its host-side
binutils runtime libraries, and static `qemu-aarch64`) are **committed**, so a
fresh clone restores offline with:

```bash
scripts/toolcache.sh restore
scripts/toolcache.sh status
```

The base image also does not ship `cmake`, `ninja` or `shellcheck`, so those
are committed as small `.deb` files under `tools/vendor/host-debs/` (19 MB) and
restored with:

```bash
scripts/toolcache.sh install-host
```

`clang` is intentionally not vendored: it pulls in a 124 MB `libLLVM.so`, over
GitHub's per-file limit. It is not required to build or test — `-Wsign-conversion`
is enabled for gcc too, so the gcc build catches the same bugs. See
`tools/vendor/host-debs/README.md` for the reasoning and `TESTING.md` §7.

See [`TESTING.md`](TESTING.md) for the full guide.

---

## Scope and non-goals

**In scope**

- User-mode x86-64 Linux binary translation to AArch64.
- Correctness-first execution with a reference interpreter as oracle.
- Measured, reproducible performance work.
- **Investigation of kernel-level compatibility** (added 2026-10-19). A user-mode
  translator cannot provide `systemd` as PID 1, containers, or other
  kernel-dependent behaviour. How far those can be reached on an unprivileged
  device is analysed in
  [`docs/architecture-feasibility.md`](docs/architecture-feasibility.md), which
  records a measured experiment and a provisional design. **Nothing of this is
  implemented, and no kernel-level code will be written until it is validated on
  the reference device.**

**Explicit non-goals (for now)**

- Running Windows or macOS binaries.
- Requiring root or KVM on the target device.
- Bit-exact cycle-accurate CPU emulation.

**Withdrawn non-goal.** "System-mode or kernel emulation" was previously listed
here as out of scope. That was withdrawn on 2026-10-19 because it was decided
without measurement and it capped the project below its stated goal. See
`DECISIONS.md` D-011.

## Contributing

Read [`CONTRIBUTING.md`](CONTRIBUTING.md) and
[`docs/agent-handoff.md`](docs/agent-handoff.md). Work in small, verified
milestones. Correctness always outranks speed.

## Licence

Apache-2.0. See [`LICENSE`](LICENSE).
