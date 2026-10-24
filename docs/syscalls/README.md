# Syscall inventory

Hyperion must eventually emulate enough of the x86-64 Linux system-call
interface for real programs to run. This directory is the authoritative,
evidence-based inventory of *which* syscalls matter and *in what order* they
should be implemented.

## The seven files

| File | Contents |
| --- | --- |
| `U4.md` | Usage tier U4 — most frequently used |
| `U3.md` | Usage tier U3 — frequently used |
| `U2.md` | Usage tier U2 — less frequently used |
| `U1.md` | Usage tier U1 — least frequently used (usage tier) |
| `E1.md` | Extra: high-value additions for x86-64/ABI compatibility |
| `E2.md` | Extra: specialized/optional subsystems |
| `E3.md` | Extra: rare, legacy, privileged or reserved |

The four usage tiers (U1–U4) rank **how likely an ordinary userspace program is
to call the syscall**, U4 highest. The three extra categories (E1–E3) classify
by **role, not frequency**. E1–E3 are not further frequency bands.

## Canonical data source

The seven Markdown files are **generated**. The single source of truth is:

- `inventory.tsv` — one row per syscall: number, name, tier, status, purpose,
  and the evidence that justified the tier.
- `kernel-table-x86_64-6.12.tsv` — the pinned x86-64 syscall table.

Do not edit the Markdown by hand. Edit `inventory.tsv` and run:

```sh
python3 tools/gen_syscall_inventory.py          # validate + regenerate
python3 tools/gen_syscall_inventory.py --check  # validate only (CI)
```

### What `status` means

| Status | Meaning | Count today |
| --- | --- | --- |
| `not-started` | the number returns `-ENOSYS` | 247 |
| `stubbed` | the number returns success but does not perform the call (e.g. `madvise`, `prctl`, `clock_getres`) | 6 |
| `implemented` | the call is performed | 122 |
| `validated` | implemented and covered by a dedicated test that also checks the failure paths | 0 |

`stubbed` exists precisely so a stub cannot be counted as support. The status is
not a self-assessment: the validator derives it from the source in both
directions — every `case sysno::kX:` in `handle_unguarded()`, every number in
`is_linux_compat_syscall()` (annotated `[stub]` or `[unsupported]`), every entry
of the compatibility layer's adapter table, and every shipped alias rule must
agree with this file. A number cannot be claimed as implemented without a
handler, and a handler cannot exist without being claimed.

The validator fails on a missing syscall, a duplicate number or name, a number
that disagrees with the pinned table, an x32 entry, an invalid tier or status,
and any syscall that the pinned table lists but the inventory does not (or vice
versa). CI runs it, so the inventory cannot silently drift from the kernel
table.

## Pinned kernel version

The reference is **Linux v6.12**,
`arch/x86/entry/syscalls/syscall_64.tbl`, fetched from the kernel tree and
reduced to the `common` and `64` ABI rows. The x32 ABI rows are excluded: they
are a distinct ABI that Hyperion does not target. The table has 375 common/64
entries (numbers 0–462, with the 336–423 gap reserved for the x32 range).

To move to a newer kernel, replace `kernel-table-x86_64-6.12.tsv`, rerun the
validator, and reconcile every reported difference before regenerating. The
validator is the mechanism that detects syscalls added to the kernel but absent
from the inventory.

## Evidence policy

Frequency claims are **not invented**. Each entry's `evidence` column is one of:

- `trace:N` — the syscall was observed `N` times across the Phase 6 strace
  sweep of representative workloads (coreutils, shells, text tools, a C
  toolchain, and a Python interpreter). This is real measurement, but it is a
  small, x86-64-host sample and is treated as a lower bound, not a definitive
  ranking.
- `provisional` — no direct measurement yet; the tier rests on the call's role
  in the ABI or its documented use.

The strace sweep is reproducible; see `docs/syscalls/evidence/` and the
`scripts/trace-syscalls.sh` helper once added. A tier is a working hypothesis to
be revised as new traces arrive, and the `status` column (not the tier) records
implementation progress: `not-started`, `stubbed`, `implemented`, `validated`.

**Current implementation status (Phase 6).** The first slice implemented
`write`(1), `exit`(60) and `exit_group`(231) in both execution tiers. The second
slice added the guest address space and `mmap`(9), `mprotect`(10), `munmap`(11)
and `brk`(12), also in both tiers. These four are marked `implemented` with a
stated scope: `mmap` supports **anonymous** mappings only (a file-backed request
returns `-ENOSYS` and is counted as unimplemented), and `munmap`/`mprotect`
require the range to be fully mapped (partial unmapping is not supported yet).
The third slice added a guest descriptor table and `read`(0), `close`(3) and
`lseek`(8). The descriptor table starts with fd 0/1/2 on the process's
stdin/stdout/stderr; an embedder can attach an already-open stream under another
guest fd with `MinimalSyscallHandler::adopt()`. `open`/`openat` are deliberately
**not** implemented, so a guest can read inherited descriptors but cannot open
host files yet. `close` drops the guest descriptor without closing the borrowed
host stream.
Every other number returns `-ENOSYS` and is counted in
`RunResult::unimplemented_syscalls`. The inventory is complete and validated, but
a complete inventory is *not* implementation; the `status` column is the only
claim of what works. See `docs/known-issues.md` LIM-002.

Frequency and implementation priority are related but not identical. A rare
syscall (e.g. `arch_prctl` for TLS setup) can be mandatory for *any* program to
start, which is why it sits in E1 rather than a low usage tier.
