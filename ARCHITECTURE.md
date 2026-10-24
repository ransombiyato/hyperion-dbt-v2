# ARCHITECTURE

This document describes the intended system design and, importantly, **which
parts exist today**. Phase 0 implements only the shaded (existing) components.

Legend: `[ ]` planned · `[x]` implemented · `[~]` partially implemented

---

## 0. Scope of this document

This document describes the **user-mode translator**: the primary architecture,
and the one the phases in [`ROADMAP.md`](ROADMAP.md) build. It assumes the
guest program runs as an ordinary host process and its syscalls are handled by
the host kernel.

That is deliberately *not* the whole compatibility story. Providing a broadly
compatible Linux execution environment — including kernel-dependent workloads
such as `systemd` as PID 1 or containers — requires kernel semantics the host
kernel will not grant an unprivileged process. The design space, the options,
and the measured evidence for how far each can go are in
[`docs/architecture-feasibility.md`](docs/architecture-feasibility.md).

The key architectural consequence for this document is a **constraint, not a
feature**: the syscall layer (§ below) must be isolated behind an interface so
that the same translator can later direct syscalls either to the host kernel or
to a user-space kernel implementation. Nothing about that second path is
implemented today.

---

## 1. Problem statement

Translate and execute x86-64 Linux user-mode binaries on an AArch64 Linux
host, correctly first and fast second. The guest and host share an OS kernel
and (usually) a page size, but differ in:

| Property | x86-64 guest | AArch64 host |
| --- | --- | --- |
| Instruction encoding | Variable length (1–15 bytes) | Fixed 4 bytes |
| General registers | 16 (RAX…R15), partial-width aliasing | 31 (X0…X30) + SP/XZR |
| Flags | One RFLAGS with arithmetic flags | NZCV; no PF/AF; conditionals are branches/selects |
| Address model | 64-bit flat | 64-bit flat |
| Memory ordering | TSO (strong) | Weak |
| Syscall ABI | `syscall`, RAX number, RDI/RSI/RDX/R10/R8/R9 args | `svc #0`, X8 number, X0–X5 args |
| W^X | Historically relaxed | Enforced |

The translation problem is dominated by three mismatches: **flags**, **partial
register writes**, and **variable-length decoding**. Each is treated explicitly
in the design below.

---

## 2. Layered design

```
                    +---------------------------------------+
                    |            CLI (src/cli)      [x]     |
                    +---------------------------------------+
                                      |
   +----------------------------------+----------------------------------+
   |                                                                     |
   |  [x] core: version, host probe, (later) logging, config             |
   |                                                                     |
   +---------------------------------------------------------------------+
        |                 |                  |                |
   [ ] decoder      [ ] IR / semantics   [ ] AArch64        [ ] runtime
   x86-64 bytes  ->  typed basic blocks  codegen         ->  cache, memory,
        |                 |                  |                dispatch
        +--------+--------+---------+--------+----------------+
                 |                 |                 |
          [ ] interpreter   [ ] JIT tiers      [ ] syscall/ABI layer
          (oracle)          (tier 1, 2)        (guest <-> host)
                                                       |
                                              [ ] library forwarding
                                              [ ] graphics forwarding (opt.)
```

### Component boundaries

| Component | Responsibility | Must not |
| --- | --- | --- |
| Decoder | bytes → instructions; never executes | know about AArch64 |
| IR / semantics | architecture-neutral meaning | allocate registers or host addresses |
| Interpreter | execute IR; correctness oracle | be optimized at the cost of clarity |
| Code generator | IR → AArch64 machine code | decide *what* to translate |
| Runtime / cache | code memory, dispatch, invalidation | interpret semantics |
| Syscall layer | guest↔host ABI translation | implement host kernel policy |
| CLI | user interface | contain translation logic |

The rule that keeps this tractable: **the decoder never knows about AArch64,
and the code generator never knows about x86-64.** They meet only at the IR.

---

## 3. Guest state representation

The x86-64 architectural state the translator must model:

```c
struct GuestState {          // illustrative; not yet implemented
    uint64_t rax, rbx, rcx, rdx;      // plus rsi, rdi, rbp, rsp
    uint64_t r8..r15;
    uint64_t rip;
    uint64_t rflags;                  // materialized lazily where possible
    // xmm0..xmm15, mmx, x87 later; segment bases; fs/gs base
};
```

**Flags strategy.** AArch64 has no parity or auxiliary flag, and x86 arithmetic
flags do not map one-to-one onto NZCV. The plan is *lazy flag materialization*:
arithmetic produces NZCV plus a small "flag source" descriptor, and the full
RFLAGS word is computed only when a guest instruction actually reads flags in a
way that needs it. Phase 2 starts with eager flags (simpler, obviously
correct) and the optimization is deferred to Phase 5 with differential tests
proving equivalence. Recorded as decision D-003.

**Partial register writes.** Writing `AL` must preserve the upper 56 bits of
`RAX`. Phase 2 will model each register as a full 64-bit value and perform
read-modify-write for partial widths; later phases may track "known high bits".

---

## 4. Execution model and tiers

```
        guest PC
           |
           v
   +-----------------+   miss    +-------------------+
   |  dispatch/hash  |---------->| decode -> IR      |
   |  (block lookup) |           | translate -> code |
   +-----------------+           +-------------------+
           | hit                          |
           v                              v
   +-----------------+          +-------------------+
   |  tier-1 code    |          |  tier-2 optimized |
   |  (fast, simple) |          |  (profiled)       |
   +-----------------+          +-------------------+
```

- **Tier 0 (Phase 2):** reference interpreter. Always available, used as the
  differential oracle for everything above it.
- **Tier 1 (Phase 3–4):** straightforward AArch64 code generation, one
  translation block per basic block, minimal register allocation. The Phase 3
  increment translated single instructions to a self-contained
  `BlockResult block(GuestState*)` block; Phase 4 added block formation
  (`translate_block` decodes to a terminator and lowers `jmp`/`jcc`) and a
  dispatcher (`TranslationCache`) that runs whole programs block by block. The
  guest registers live in memory and are loaded/stored by the generated code,
  and every x86 flag is recomputed from AArch64 NZCV (D-017).
- **Tier 2 (Phase 5+):** optimized blocks for hot paths — better register
  allocation, constant propagation, dead-code elimination, block chaining.

Tier promotion is driven by measured execution counts, never speculation.
Every tier must produce bit-identical guest state; this is enforced by tests.

---

## 5. Translation cache and invalidation

**Implemented (Phase 4):** `TranslationCache` (`translation_cache.hpp`) is a
hash map keyed by guest PC holding one generated block per entry. The
dispatcher loop is:

```
while not halted and steps < limit:
    block = lookup(state.rip) or translate_block(state.rip)
    if block is unsupported: stop with kUnsupported, rip left on the instruction
    run block(state)          # block writes the authoritative next rip
```

Design properties that are deliberate at this stage:

- The **authoritative next PC is `GuestState::rip`**, written by the generated
  block. The cache is only a lookup accelerator; a missing or stale entry can
  never change what the program computes.
- **Unsupported instructions stop the run.** The block cutter stops *before* an
  instruction it cannot translate, so the dispatcher sees that PC on the next
  lookup and reports it. A block whose first instruction is unsupported is
  reported as an error rather than emitted empty (which would loop forever).
- Generated code lives in `ExecMem` arenas: written `PROT_WRITE`, then made
  `PROT_READ|PROT_EXEC` with an instruction-cache clear. **W^X is assumed on
  AArch64** (probed at runtime, see `host_probe`).
- The cache **never invalidates** in this version. Self-modifying guest code
  runs stale translations. This is a documented boundary, not an accident;
  precise invalidation (guest page → block set) is Phase 7 work.

Still planned (Phase 5+):

- Fixed-size, hash-indexed lookup with a generation tag instead of `unordered_map`.
- Block chaining so a block exit jumps straight to its successor instead of
  returning to the dispatcher.
- Invalidation: a mapping from guest page → set of blocks, so self-modifying
  code or `mprotect` invalidates precisely.

---

## 6. Syscall and ABI layer (Phase 6)

Guest `syscall` instructions are intercepted and translated. The layer owns:

- argument register marshalling (`RDI,RSI,RDX,R10,R8,R9` → no host register at
  all: HYPERION performs the operation itself or calls the host with translated
  arguments);
- structure layout differences (`stat`, `statx`, `epoll_event`, `pollfd`,
  `rusage`, `tms`, `itimerval`, `timespec`, `fd_set`, `iovec`);
- guest-to-host **file descriptor translation** — a guest integer never names a
  host descriptor;
- memory validation of every guest pointer against the `GuestAddressSpace`
  before it is dereferenced;
- guest signal *handler invocation*, thread creation, futexes and `rseq` (see
  the status matrix in `docs/syscalls/`); none of these is claimed beyond what
  the tests show.

A guest syscall number is **never** forwarded to the host kernel. There is no
`syscall(number, ...)` passthrough anywhere in the layer: the number selects one
of a fixed set of implementations, and an unknown number returns `-ENOSYS`.

### 6.1 Dispatch pipeline

`MinimalSyscallHandler::handle()` is the single entry point both tiers call. It
runs four layers in a fixed order, and each layer is cheaper to ask than the next
one is to run:

| # | Layer | Cost when it answers | What it may do |
| --- | --- | --- | --- |
| 1 | Compiled rule table (`SyscallDispatcher::lookup`) | one bounds check + one array index | call a **whitelisted adapter** by id, or rewrite the argument registers for an alias |
| 2 | `handle_unguarded()` switch | a jump table on the syscall number | the statically implemented syscalls |
| 3 | Legacy `linux_compat_` switch | only reached for numbers layer 2 does not claim | the older `enable_linux_compat()` implementations |
| 4 | Refusal | plain `-ENOSYS` | count the number as unsupported, for diagnostics only |

The rule table is a `std::array` indexed by syscall number up to
`kMaxSyscall = 512`, so layer 1 is constant time and allocation-free. There is
**no** JSON parsing, filesystem access, lock, allocation, or string hashing on
this path: the registry is parsed once, `reload()` compiles it into an immutable
`CompiledRules` snapshot held in a `shared_ptr`, and dispatch only ever reads
that snapshot. A concurrent `reload()` swaps the pointer; a syscall already in
flight keeps using the snapshot it started with.

The layers are ordered so that the static implementation always wins: a registry
rule for a number that layer 2 or 3 implements is **rejected at compile time**
with a diagnostic, rather than silently shadowing tested code. Rules can only
add syscalls HYPERION does not otherwise answer.

### 6.2 What an adapter is, and what it is not

An adapter is a C++ function identified by a name from a fixed table
(`kAdapterTable` in `src/core/syscall_adapters.cpp`). The registry may name an
adapter; it may not name a host syscall number, a library, a shell command or an
expression. A rule's `handler` field is looked up in that table and an unknown
handler name is a validation error, so the JSON file cannot widen the set of
things HYPERION executes — only choose among implementations that are compiled
into the binary and covered by the test suite.

Each adapter declares the argument `transforms` it performs (for example
`packed_epoll_event`, `fd_set`, `pollfd_array`). The declaration is checked
against the compiled list, and a mismatch between what a rule claims and what the
adapter does is a compile error. `return_transform` must be `errno_negative`,
because a negative-`errno` return is the only convention the guest ABI allows.

### 6.3 The persistent registry

`~/.local/share/hyperion/syscalls/registry.json`
(`$XDG_DATA_HOME/hyperion/syscalls/registry.json` when set; Termux on Android
uses the same resolution) holds the user's rules. It lives outside the install
directory so that upgrading HYPERION cannot overwrite it, and `install.sh`
creates it only when it is absent.

- **Resolution order:** `--syscall-registry=FILE` → `$HYPERION_SYSCALL_REGISTRY`
  → `$XDG_DATA_HOME` → `$HOME/.local/share`. The chosen path and the reason are
  printed by `hyperion syscall-registry --list`, so there is never a question
  about which file is in effect.
- **Safety:** the reader caps the file at 16 MiB, validates every field, rejects
  unknown handlers and out-of-range numbers, and writes atomically (temp file +
  `fsync` + `rename` + directory `fsync`, previous contents kept as `.bak`,
  files `0600`, directories `0700`).
- **Failure is never fatal:** a missing, unreadable, corrupt or newer-schema file
  leaves the built-in rules in force and never prevents startup. A file is never
  rewritten as a side effect of reading it.
- **Schema migration:** v1 documents are migrated to v2 in memory on read and
  are not written back unless the user asks.
- **Provenance:** every rule carries an `origin` (`builtin`, `package`, `user`)
  so package-shipped defaults can be updated without touching user rules, and
  the shipped defaults are merged by rule id.
- **Children inherit the snapshot, they do not re-read the file.** `fork(2)`,
  `vfork(2)` and the process-creating `clone(2)` produce a child with its own
  syscall handler; that child adopts the parent's compiled rules
  (`SyscallDispatcher::adopt_snapshot`). Reloading would be both slow and racy
  against a reload in the parent, and doing nothing — the original behaviour —
  meant a user rule worked before `fork` and silently returned `-ENOSYS` after
  it. Counters and the unsupported set stay per-process, because they describe
  what that process did.

### 6.4 Observability

`SyscallCounters` records, per run, how many calls were answered by each layer,
how many translations failed, and the set of unsupported numbers with their
first-seen order. The summary line is printed by `hyperion run`:

```
syscall dispatch: adapter=8 alias=0 existing=18 unsupported=1 translation_failures=0
unsupported syscalls: 1 unique
  232 (0xe8) <unknown>
registry: /home/user/.local/share/hyperion/syscalls/registry.json (40 adapter rule(s), 2 alias rule(s))
```

`unsupported` counts **events**; the unique list below it counts **numbers**.
The older `unimplemented syscalls: N` line that the interpreter and translation
cache print has always counted events (each occurrence of a refused call), not
distinct numbers, and it includes refusal of a syscall that a *later* layer then
handles successfully — that is why the dispatch summary separates the two.
`HYPERION_TRACE_SYSCALLS=1` logs every call with its number, result and host pid
via `write(2)`, so tracing cannot corrupt the guest's own buffered output.

Compatibility is claimed only via the feature matrix in
[`COMPATIBILITY.md`](COMPATIBILITY.md) backed by integration tests, and the
coverage counts in [`STATUS.md`](STATUS.md) are generated from
`docs/syscalls/inventory.tsv` through `tools/gen_syscall_inventory.py --check`.
A handful of working programs does **not** constitute "Linux compatibility".

---

## 7. Reuse strategy (see Phase 1)

HYPERION will not reimplement mature, license-compatible components where
reuse is safer. Phase 1 produces a written comparison of FEX-Emu, Box64,
QEMU TCG and relevant decoder/compiler projects, their licences and their
trade-offs. `THIRD_PARTY.md` is the licence inventory. No third-party code is
vendored until that analysis is committed.

---

## 8. Safety boundaries

- Guest code runs **only** inside the translator, in-process, with no elevated
  privileges. The translator never requires root or KVM.
- Generated code is produced by our own code generator from validated IR; the
  translator does not `exec` guest binaries directly.
- Host library forwarding is opt-in and allow-listed (Phase 9).
- Details and the full threat model: [`SECURITY.md`](SECURITY.md).

---

## 9. Directory-to-component mapping

| Directory | Component | Phase introduced |
| --- | --- | --- |
| `src/core/` | all core code today: version, host probe, guest state, decoder, interpreter, AArch64 encoder, executable memory, translator, translation cache, address space, runtime helpers (soft MMU), syscall layer | 0–6 |
| `src/cli/` | user interface | 0 |
| `tests/`, `benchmarks/` | verification | 0 |

**Reality note.** The component table in §2 names the logical components; the
tree currently keeps all of them in `src/core/` (`decoder.cpp`,
`interpreter.cpp`, `aarch64_encoder.cpp`, `exec_mem.cpp`, `translator.cpp`,
`translation_cache.cpp`, `runtime_helpers.cpp`, `address_space.cpp`,
`syscall.cpp`). Splitting into
`src/decoder/`, `src/codegen/` etc. was the original plan but was
not done, and the project does not pretend otherwise. The split is deferred
until the translator grows beyond single instructions and the boundaries are
exercised enough to place the cut well; until then a flat tree is easier to
navigate than a premature one.
