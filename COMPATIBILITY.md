# COMPATIBILITY

Tracks implemented versus missing instruction and ABI features.

> **Status:** a small, documented instruction subset is implemented and tested
> (§2). Everything else is explicitly rejected. AArch64 code generation, a
> block-at-a-time translation cache, a guest address space, and ten syscall
> slices (`write`/`exit`/`exit_group`; `mmap`/`mprotect`/`munmap`/`brk`;
> `read`/`close`/`lseek` over a guest descriptor table; `openat`/`fstat`; the
> process-identity getters plus `getcwd`/`getrandom`; `newfstatat`; `access`
> and `readlink`; `readlinkat`; `getdents64`; `statx`) exist;
> register allocation and memory operands (MOV, ALU read-modify-write and
> INC/DEC, via the soft-MMU runtime helpers) are implemented in the code
> generator, while block chaining does not (§5).

Legend: ✅ implemented + tested · 🟡 implemented, untested · 🧪 prototype · ❌ unsupported (explicit) · ⬜ planned

---

## 1. Tracking rules

1. A feature is ✅ only when it has **dedicated automated tests** that pass.
2. A feature implemented but not yet covered by tests is 🟡 and must be listed
   in `docs/known-issues.md`.
3. Unsupported instructions must **fail explicitly** at translation time with a
   clear diagnostic. Silent misinterpretation is a correctness bug of the
   highest severity.
4. Every row states the phase in which it is expected to land.

---

## 2. Instruction subset — implemented (Phase 2)

This section is the **contract** the Phase 3 code generator must meet. It
describes what `src/core/decoder.cpp` accepts *today*, verified by
`tests/unit/test_decoder.cpp` and `tests/differential/`. Anything not listed
here is rejected with a specific `DecodeError` — the decoder never guesses.

### 2.1 Supported instructions

| Instruction | Encodings accepted | Operand sizes | Notes |
| --- | --- | --- | --- |
| `mov r, r/m` | `0x8B` | 32, 64 | reg ← reg/mem (incl. `[rip + disp32]`) |
| `mov r/m, r` | `0x89` | 32, 64 | reg/mem ← reg (incl. `[rip + disp32]`) |
| `mov r, imm` | `0xB8+rd` | 32, 64 | 32-bit imm; 64-bit form reads 8-byte imm |
| `lea r, m` | `0x8D` | 32, 64 | `[rip + disp32]` & full SIB; `mod=11` rejected; Tier 0 & Tier 1 |
| `add` | `0x01`, `0x03`, `0x05`, `0x81/0`, `0x83/0` | 32, 64 | r/m,r · r,r/m · eax/rax,imm32 · imm32 · imm8(sx) |
| `or` | `0x09`, `0x0B`, `0x0D`, `0x81/1`, `0x83/1` | 32, 64 | as above |
| `and` | `0x21`, `0x23`, `0x25`, `0x81/4`, `0x83/4` | 32, 64 | as above |
| `sub` | `0x29`, `0x2B`, `0x2D`, `0x81/5`, `0x83/5` | 32, 64 | as above |
| `xor` | `0x31`, `0x33`, `0x35`, `0x81/6`, `0x83/6` | 32, 64 | as above |
| `cmp` | `0x39`, `0x3B`, `0x3D`, `0x81/7`, `0x83/7` | 32, 64 | flags only; no write-back |
| `test` | `0x85`, `0xA9`, `0xF7/0` | 32, 64 | r/m,r · eax/rax,imm32 · r/m,imm32; Tier 0 & Tier 1 |
| `inc` | `0xFF/0` | 32, 64 | preserves CF |
| `dec` | `0xFF/1` | 32, 64 | preserves CF |
| `push r64` | `0x50+rd` | 64 | `rsp -= 8; [rsp] = r` |
| `pop r64` | `0x58+rd` | 64 | `val = [rsp]; rsp += 8; r = val` (`pop rsp` sets popped value) |
| `jmp rel32` | `0xE9` | — | absolute target = next rip + rel32 |
| `jmp rel8` | `0xEB` | — | short form; absolute target = next rip + rel8 |
| `jcc rel8` | `0x70..0x7F` | — | all 16 conditions (short form) |
| `jcc rel32` | `0x0F 0x80..0x8F` | — | all 16 conditions (near form); Tier 0 & Tier 1 |
| `syscall` | `0x0F 0x05` | — | 42 Linux syscalls handled; `-ENOSYS` otherwise |
| `nop` | `0x90` | — | benign REX (`REX.B == 0`) accepted; `REX.B == 1` (`xchg r8, rax`) rejected |
| `hlt` | `0xF4` | — | stops execution (see §2.4) |
| `ret` | `0xC3` | — | modelled as "stop" (see §2.4) |

For the `0x05..0x3D`, `0x81`/`0x83`, `0xA9`, and `0xF7/0` immediate forms with
a 64-bit operand, the immediate is **sign-extended** to 64 bits (`imm32`→64,
`imm8`→64), exactly as x86-64 does. Instructions exceeding 15 bytes return
`DecodeError::kTooLong`. `0x80` (8-bit group 1) is rejected.

### 2.2 Addressing forms

`ModRM` mod=00/01/10 with rm≠100 (no SIB), rm=101/mod=00 (`[rip + disp32]`), and
rm=100 (SIB, including `mod=00, base=101` `[disp32]` / `[index*scale + disp32]`)
are all supported:

- `[rip + disp32]` (resolved against `insn.address + insn.length + disp32`; `REX.B` ignored per Intel SDM Vol. 2A Table 2-7)
- `[disp32]`, `[index*scale + disp32]` (`SIB mod=00, base=101`)
- `[base]`, `[base + disp8]`, `[base + disp32]`
- `[base + index*scale + disp8/disp32]` with scale ∈ {1, 2, 4, 8}
- REX.R/REX.X/REX.B extend the reg/index/base fields to r8–r15.

**Not supported (rejected):** 32-bit address-size override (`0x67`) and 8-bit (`0x88`/`0x8A`) forms.

### 2.3 Rejection behaviour (`DecodeError`)

| Error | Raised when |
| --- | --- |
| `kTruncated` | the instruction extends past the supplied bytes |
| `kUnknownOpcode` | opcode not in the supported set (incl. `0F`-map, `adc`/`sbb`) |
| `kUnsupportedPrefix` | `0x66`, `LOCK`, and other prefixes not modelled |
| `kUnsupportedOperand` | operand form outside the subset (e.g. 8-bit mov) |
| `kUnsupportedModrm` | ModRM/SIB combination not supported yet (e.g. RIP-relative) |

A rejected instruction must leave guest state **unchanged** and `rip`
**unmoved**; `tests/unit/test_interpreter.cpp` asserts this.

### 2.4 Deliberate simplifications

- `hlt` and `ret` both stop the interpreter and report `kHalted`. Real
  user-mode x86-64 would `#GP` on `hlt` and would pop a return address on
  `ret`. These are modelling choices for the Phase 2 test harness, recorded
  here so they are not mistaken for correct behaviour. They will be revisited
  in Phase 4 (block exits) and Phase 6 (signals).
- `ret` does **not** pop the stack.

### 2.5 Planned for Phase 2 but NOT implemented

These appeared in an earlier version of this table and remain unbuilt. They are
**not** covered by any test:

| Group | Instructions | Status |
| --- | --- | --- |
| `test` | `0x85` | ⬜ planned |
| Unary | `neg`, `not` | ⬜ planned |
| Shift/rotate | `shl`, `shr`, `sar` (imm and `CL`) | ⬜ planned |
| Multiply | `imul`, `mul` | ⬜ planned |
| Address | `lea` | ⬜ planned |
| Control flow | `call`, `ret` (real), `jcc rel32` (`0F 8x`) | ⬜ planned |
| Syscall | `syscall` (`0F 05`) | ✅ decoded + handled (§5) |
| Division | `div`, `idiv` | ⬜ planned (stretch) |
| 8-bit forms | `mov r8`, `add r8`, … | ⬜ planned |

**Out of Phase 2 scope entirely:** SIMD/SSE/AVX, x87, MMX, atomics,
`rep`-prefixed string ops, segmentation, and most system instructions. All are
rejected explicitly.

### 2.6 AArch64 translation coverage (Phase 3–4)

The translator lowers a subset of the decoder's subset. An instruction the
decoder accepts but the translator does not is refused with an explicit
`TranslateError`; nothing is miscompiled. Phase 4 adds whole-block translation
(`translate_block`) and a dispatcher (`TranslationCache`).

| Instruction | Translated? | Notes |
| --- | --- | --- |
| `mov` | ✅ | register and immediate, 32/64-bit; 32-bit zero-extends. **Memory forms** (`mov r64, [mem]` / `mov [mem], r64` / the 32-bit forms) lower to a runtime-helper call (soft MMU); a fault exits the block with `kMemoryFault` |
| `add`, `sub` | ✅ | register and immediate, 32/64-bit; full flag set |
| `and`, `or`, `xor` | ✅ | register and immediate; CF/OF/AF cleared |
| `cmp` | ✅ | flags only, no writeback |
| `inc`, `dec` | ✅ | CF preserved across the operation |
| `nop` | ✅ | no effect |
| `hlt` | ✅ | block exits with `kHalted`; `rip` not advanced |
| `jmp rel32` (`0xE9`) | ✅ | block exits with `kJmp`; writes target `rip` |
| `jmp rel8` (`0xEB`) | ✅ | block exits with `kJmp`; writes target `rip` |
| `jcc rel8` (`0x70..0x7F`) | ✅ | block exits with `kJcc`; writes target or fall-through `rip` |
| `push`, `pop`, `ret` | ⬜ | needs a guest stack / address space (Phase 6) |
| `syscall` (`0F 05`) | ✅ | block exits with `kSyscall`; the dispatcher runs the handler. `rip` is advanced past the instruction |
| ALU memory operand (`add r, [m]`, `add [m], r`, `inc/dec [m]`) | ✅ | **Lowered to the soft-MMU helpers**: load, apply the operation on registers, store back (or, for `cmp`, flags only). A load or store fault exits the block as `kMemoryFault` with `rip` on the faulting instruction, matching the interpreter. Verified by `translator.differential_alu_memory_operands` (registers, all six flags and the memory word) and `..._fault_is_reported`, both executed under `qemu-user` |
| Two memory operands (e.g. `mov [m], [m]`) | ❌ | x86-64 has no such form; `validate_body` refuses it as `kUnsupportedOperand` rather than guessing |

Verified by `tests/unit/test_translator.cpp`: scope tests on every host, a
single-instruction differential against the interpreter under `qemu-user` on
AArch64 (847 operand cases plus a fixed instruction set), **and whole-program
differentials** — a counted loop, taken/not-taken branches, a memory program
(store/load round trip, 32-bit forms, and a fault case), and the ALU
read-modify-write chain with its fault case — that compare the final
`GuestState`, step count and memory effects against `run_program`. The
`hyperion run --tier=1` CLI runs a complete program through generated AArch64
code and is required to agree with tier 0 (`tests/cli/cli_smoke.sh`).

---

## 3. Flag coverage

The interpreter implements **eager** flag computation (D-003) for six flags.
Lazy materialization is a Phase 5 optimization. Each flag below is verified
against real x86-64 hardware by `tests/differential/`, and the Phase 3
translator recomputes all six in generated AArch64 code (D-017), checked
against the interpreter by `tests/unit/test_translator.cpp`.

| Flag | Meaning | Status | Notes |
| --- | --- | --- | --- |
| CF | Carry / borrow | ✅ | set by add/sub; cleared by logic ops; preserved by inc/dec |
| ZF | Zero | ✅ | set by all modelled ALU ops |
| SF | Sign (bit 31/63) | ✅ | set by all modelled ALU ops |
| OF | Signed overflow | ✅ | set by add/sub/inc/dec; cleared by logic ops |
| PF | Parity of low byte | ✅ | `__builtin_parity`; no AArch64 equivalent, must be computed |
| AF | Auxiliary carry (bit 3) | ✅ | no AArch64 equivalent, must be computed |
| DF | Direction | ⬜ | only needed for string ops (Phase 7) |
| IF/TF/IOPL | System flags | ⬜ | not modelled; user-mode subset only |

`pack_flags`/`unpack_flags` map these to/from architectural RFLAGS bits
(CF=0, PF=2, AF=4, ZF=6, SF=7, OF=11), and bit 1 is always set as on real
hardware. Unmodelled bits are ignored on unpack and dropped on pack — this is
intentional and tested.

---

## 4. Register and state coverage

| State | Status | Notes |
| --- | --- | --- |
| GPR r64 (RAX…R15) | ✅ | full 64-bit values; index order matches x86-64 encoding |
| GPR 32-bit aliases | ✅ | 32-bit writes zero-extend to 64 (tested against hardware) |
| GPR 16/8-bit aliases | ⬜ | not modelled; 8-bit mov is rejected, no 16-bit prefix |
| RIP | ✅ | advanced per instruction; branches set an absolute target |
| RFLAGS | ✅ | six flags (CF/PF/AF/ZF/SF/OF), eager; see §3 |
| RSP / stack | 🟡 | `push`/`pop` work over `FlatMemory`; no guest stack mapping yet |
| XMM / SSE | ⬜ | Phase 7 |
| x87 / MMX | ⬜ | Phase 7, likely via reuse |
| FS/GS base, segment bases | ⬜ | Phase 6 (TLS) |
| MXCSR | ⬜ | Phase 7 |

`FlatMemory` is a single flat region used by the interpreter's tests. It is
**not** the guest address space. The real guest address space is
`GuestAddressSpace` (`include/hyperion/address_space.hpp`): a set of regions with
per-region permissions (`MemPerm`) and page-granular checks, plus a reserved heap
window for `brk`. It is wired into the syscall layer, so `mmap`/`mprotect`/
`munmap`/`brk` operate on it. There is still no paging, no demand-zero fault
delivery to the guest, and no mapping of the guest stack; those arrive later in
Phase 6/7.

---

## 5. Linux ABI / syscall coverage

Phase 6, tenth slice. The `syscall` instruction is decoded and routed through
`SyscallHandler` in both tiers. **Twenty-seven syscalls are implemented** (`read`,
`write`, `close`, `fstat`, `lseek`, `mmap`, `mprotect`, `munmap`, `brk`,
`getpid`, `exit`, `getcwd`, `getuid`, `getgid`, `geteuid`, `getegid`, `getppid`,
`gettid`, `getdents64`, `exit_group`, `openat`, `newfstatat`, `access`,
`readlink`, `readlinkat`, `statx`, `getrandom`); every other number returns
`-ENOSYS` explicitly (never a
silent success) and is counted in `RunResult::unimplemented_syscalls`. Guest
pointers are validated against the mapped region (`-EFAULT`), so a guest can
never read host memory. Per-syscall status lives in `docs/syscalls/inventory.tsv`
(the canonical source); this table is the summary.

`read`/`close`/`lseek`/`openat`/`fstat` operate on a **guest descriptor table**
owned by the handler, not on host file descriptors: the handler maps guest fd →
host stream and refuses a guest fd that is not in the table with `-EBADF`, so a
guest cannot name an arbitrary host descriptor. The table starts with fd 0/1/2 on
the process's stdin/stdout/stderr, and an embedder can attach an already-open
stream under another guest fd with `MinimalSyscallHandler::adopt()`. `openat`
allocates the lowest free guest fd (as the kernel does) and the stream it opens
is handler-owned, so `close` releases it; a stream attached with `adopt()` is
borrowed and never `fclose`d by the handler.

`openat` is limited to `AT_FDCWD` and a bounded guest pathname. Any other `dirfd`
(a real `*at()`), or an `O_*` flag outside the honoured set, returns `-ENOSYS`
and is counted as unimplemented — never resolved against the wrong base or given
weaker semantics than the guest asked for. **This is not a sandbox**: `openat`
forwards to the host kernel, so the guest can open whatever the host process may
open (SECURITY.md).

`fstat` reports the **guest** (x86-64) `struct stat` layout, built field-by-field
from the host's rather than `memcpy`'d: the host layout differs on AArch64, so a
copy would put values at the wrong offsets. `newfstatat`(262) shares that same
builder, so the ABI-sensitive layout is written in exactly one place;
`AT_SYMLINK_NOFOLLOW` selects `lstat` semantics, and any other flag (e.g.
`AT_EMPTY_PATH`) or a real `dirfd` returns `-ENOSYS` and is counted as
unimplemented.

`mmap` is **anonymous-only**: a request without `MAP_ANONYMOUS` is reported
unimplemented (and counted as such), not mapped incorrectly. `mprotect`/`munmap`
require a page-aligned, fully-mapped range. For `MAP_ANONYMOUS` the kernel
ignores `fd`/`offset`; Hyperion matches that rather than inventing a stricter
rule.

The **process-identity getters** (`getpid`/`gettid`/`getuid`/`geteuid`/`getgid`/
`getegid`/`getppid`) and `getcwd`/`getrandom` are pure forwards to the host
process. Hyperion runs the guest in-process rather than as a separate guest
kernel, so the guest sees the host's ids; for a single-process guest this is
indistinguishable from the truth. `gettid` forwards to the host, which is exact
for the single guest thread that exists today; it must be revisited if thread
support is ever added (`clone` with `CLONE_THREAD` is currently refused, so no
guest can create a second thread). `getcwd` copies the host working directory
into a bounded guest buffer (`-ERANGE` when the path plus NUL does not fit,
`-EFAULT` on a bad pointer, `-EINVAL` for size 0). `getrandom` fills a bounded
guest buffer from the host in 4 KiB chunks, accepting only the two defined
`GRND_*` flag bits and rejecting any other bit with `-EINVAL`.

| Area | Status | Phase | Covering test |
| --- | --- | --- | --- |
| Syscall number + argument marshalling (`rdi,rsi,rdx,r10,r8,r9` → `rax`) | ✅ | 6 | `syscall.*` |
| `exit`(60), `exit_group`(231) | ✅ | 6 | `syscall.exit_reports_code_*`, `interpreter.syscall_write_then_exit_*` |
| `write`(1) | ✅ | 6 | `syscall.write_*`, `translation_cache.tier1_syscall_*` |
| `read`(0) over a guest descriptor table | ✅ | 6 | `syscall.read_copies_from_adopted_stream_*`, `syscall.read_short_read_*`, `syscall.read_*_ebadf`/`_efault` |
| `close`(3) | ✅ | 6 | `syscall.close_drops_the_descriptor_but_not_the_stream`, `syscall.close_of_bad_fd_is_ebadf` |
| `lseek`(8) | ✅ | 6 | `syscall.lseek_repositions_*`, `syscall.lseek_end_and_cur_whence_work`, `syscall.lseek_bad_whence_*` |
| `openat`(257) (`AT_FDCWD`, bounded path, honoured flags) | ✅ (scoped) | 6 | `syscall.openat_opens_a_real_file_*`, `syscall.openat_missing_file_is_enoent`, `syscall.openat_bad_path_pointer_is_efault`, `syscall.openat_with_a_real_dirfd_is_unimplemented`, `syscall.openat_unhandled_flag_is_unimplemented`, `syscall.openat_writes_and_closes_release_the_guest_fd` |
| `fstat`(5) in the guest `struct stat` layout | ✅ | 6 | `syscall.fstat_reports_size_in_the_guest_layout`, `syscall.fstat_bad_fd_is_ebadf_and_bad_buffer_is_efault` |
| `newfstatat`(262) (`AT_FDCWD`, `AT_SYMLINK_NOFOLLOW`, shared guest `stat` layout) | ✅ (scoped) | 6 | `syscall.newfstatat_reports_path_metadata_in_the_guest_layout`, `syscall.newfstatat_missing_path_is_enoent`, `syscall.newfstatat_bad_path_pointer_is_efault`, `syscall.newfstatat_unmapped_buffer_is_efault`, `syscall.newfstatat_with_a_real_dirfd_or_bad_flag_is_unimplemented`, `syscall.newfstatat_symlink_nofollow_uses_lstat` |
| `getpid`(39)/`gettid`(186)/`getuid`(102)/`geteuid`(107)/`getgid`(104)/`getegid`(108)/`getppid`(110) | ✅ | 6 | `syscall.identity_getters_forward_to_the_host` |
| `getcwd`(79) | ✅ | 6 | `syscall.getcwd_copies_the_host_working_directory`, `syscall.getcwd_too_small_is_erange_and_bad_buffer_is_efault` |
| `getrandom`(318) | ✅ | 6 | `syscall.getrandom_fills_the_guest_buffer`, `syscall.getrandom_zero_length_is_zero_and_bad_flags_is_einval` |
| `access`(21) | ✅ (host permissions) | 6 | `syscall.access_reports_a_real_file_and_enoent_for_a_missing_one`, `syscall.access_bad_mode_is_einval_and_bad_path_pointer_is_efault` |
| `readlink`(89) | ✅ (scoped) | 6 | `syscall.readlink_copies_the_target_without_a_terminator`, `syscall.readlink_truncates_a_long_target_and_rejects_zero_size` |
| `readlinkat`(267) | ✅ (scoped) | 6 | `syscall.readlinkat_with_at_fdcwd_matches_readlink`, `syscall.readlinkat_with_a_real_dirfd_is_unimplemented`, `syscall.readlinkat_rejects_zero_size_and_bad_pointer` |
| `getdents64`(217) | ✅ | 6 | `syscall.getdents64_lists_a_directory_through_openat`, `syscall.getdents64_on_a_regular_file_is_enotdir`, `syscall.getdents64_bad_fd_is_ebadf_and_small_count_is_einval`, `syscall.getdents64_bad_buffer_is_efault`, `syscall.openat_accepts_the_directory_flags` |
| `statx`(332) | ✅ (scoped) | 6 | `syscall.statx_reports_a_real_file_in_the_guest_layout`, `syscall.statx_missing_path_is_enoent`, `syscall.statx_bad_buffer_is_efault_and_bad_pointer_is_efault`, `syscall.statx_with_a_real_dirfd_is_unimplemented`, `syscall.statx_unhandled_flag_is_unimplemented` |
| Unknown number → `-ENOSYS` | ✅ | 6 | `syscall.unknown_number_is_enosys_not_silent` |
| `brk`(12) over the guest heap | ✅ | 6 | `syscall.brk_returns_the_new_break` |
| `mmap`(9) anonymous, `mprotect`(10), `munmap`(11) | ✅ (anonymous only) | 6 | `syscall.mmap_*`, `syscall.mprotect_*`, `syscall.munmap_*`, `syscall.mmap_end_to_end_*` |
| `open`(2, legacy), `openat2`(437) | ⬜ | 6 | — |
| `stat`(4)/`lstat`(6) as distinct entry points (glibc uses `newfstatat`) | ⬜ | 6 | — |
| `faccessat`(269)/`faccessat2`(439) | ⬜ | 6 | — |
| `mmap` file-backed, `MAP_SHARED` | ⬜ | 6 | — |
| `arch_prctl` (FS/GS base, TLS) | ⬜ | 6 | — |
| `set_tid_address`, `rseq`, `prlimit64` | ⬜ | 6 | — |
| `fork`(57) | ✅ | 6 | `guest.process` (`fork + waitpid exit status`) |
| `vfork`(58) | ✅ (parent suspends; address space **not** shared) | 6 | `guest.process` (`vfork`) |
| `clone`(56) — process-creating subset | ✅ (partial) | 6 | `guest.process` (`raw clone: unsupported flags refused with ENOSYS`) |
| `clone`(56) — `CLONE_VM`/`CLONE_THREAD`/`CLONE_FILES`/`CLONE_FS`/`CLONE_SIGHAND` | ❌ `-ENOSYS` by design | 6 | `guest.process` (same test) |
| `clone`(56) — namespace flags (`CLONE_NEWNS`/`NEWPID`/`NEWUSER`/…) | ❌ `-ENOSYS` (needs `CAP_SYS_ADMIN`) | 6 | `guest.process` (same test) |
| `execve`(59) | ✅ | 6 | `guest.process` (`execve of a second program`) |
| `wait4`(61) | ✅ (`WNOHANG`/`WUNTRACED`/`WCONTINUED`, `struct rusage`) | 6 | `guest.process` (`fork + waitpid exit status`) |
| `waitid`(247) | ⬜ (`siginfo_t` layout differs per arch; BusyBox and glibc use `wait4`) | 6 | — |
| `pipe`(22)/`pipe2`(293) + inheritance across `fork` | ✅ | 6 | `guest.process` (`pipe + fork IPC`, `file descriptor inheritance`) |
| `futex`, threading (multi-threaded guests) | ⬜ | 6 | — |
| Signals: `rt_sigaction`/`rt_sigprocmask` recorded; guest handlers **not** invoked | ⚠️ partial | 6 | — |
| `epoll`, `poll`, `select` | ⬜ | 6 | — |
| `io_uring`, `rseq`, `vDSO` | ⬜ | 6/7 | — |

---

## 5.05 Process creation — what is actually supported

A guest process is a **real host process** plus its own private copy of the guest
execution state. `fork(2)` performs a real host `fork()`; the child runs the guest
from the instruction after the `syscall` and terminates its host process with the
guest's exit status; the parent records the child and reaps it with `wait4(2)`.

Three implementation facts make this safe, and all three are load-bearing:

1. **Both execution tiers advance `rip` past the `syscall` instruction before
   invoking the handler** (`interpreter.cpp` `Opcode::kSyscall`,
   `translation_cache.cpp` `BlockResult::kSyscall`), so a child resumes at the
   right place with no program-counter fixup.
2. **Guest pages are backed by host heap storage owned by `GuestAddressSpace`.**
   A host `fork()` alone would leave parent and child sharing those same vectors
   until either side `mmap`/`munmap`s and reallocates them, invalidating the
   other's cached host page pointers. The child therefore receives a *deep copy*
   (`GuestAddressSpace`'s copy constructor).
3. **The parent discards its translated code before resuming after a fork**
   (`process.cpp` `do_fork_common` bumps `code_epoch()`, which makes
   `TranslationCache` drop every block and re-translate). Every block in the
   cache was produced before the process split; returning to it afterwards
   faulted intermittently on an AArch64 host — about one run in ten for a
   forking guest, with a different case failing each time. The fault also
   discarded the guest's buffered stdout, so the symptom was an empty result
   with exit status 139 rather than an error message. Native x86-64 never
   showed it (0 failures in 200 runs), so it is invisible in the host test
   runs and only appears on the AArch64 target.

| Behaviour | Status |
| --- | --- |
| `fork()` returns 0 in the child, child pid in the parent | ✅ |
| Child exit status reaches the parent through `wait4`/`waitpid` | ✅ |
| Descriptor inheritance across `fork`, with a **shared file offset** | ✅ |
| `FD_CLOEXEC` honoured on `execve` (per-descriptor, not per-OFD) | ✅ |
| `pipe`/`pipe2` + `fork` inter-process communication | ✅ |
| `execve` of another x86-64 ELF64, with `PATH` search for bare names | ✅ |
| `execve` leaves the process untouched on every error path | ✅ |
| `vfork` suspends the parent until the child execs or exits | ✅ |
| `vfork` shares the address space | ❌ **not** — the child gets a private copy (see below) |
| `CLONE_CHILD_SETTID` / `CLONE_CHILD_CLEARTID` / `CLONE_PARENT_SETTID` | ✅ |
| `CLONE_SETTLS` (child `%fs` base) | ✅ |
| Thread creation (`CLONE_VM`, `CLONE_THREAD`, `CLONE_FILES`, `CLONE_FS`, `CLONE_SIGHAND`) | ❌ `-ENOSYS` |
| Namespace creation (`CLONE_NEWNS`/`NEWUTS`/`NEWIPC`/`NEWUSER`/`NEWPID`/`NEWNET`) | ❌ `-ENOSYS` |
| `waitid`(247) | ❌ not implemented (`-ENOSYS`) |
| Guest-installed signal handlers invoked on `SIGCHLD` | ❌ not invoked |

### Deliberate divergences from Linux

These are choices, not gaps, and each is pinned by a test so it cannot silently
regress into a fake success.

* **`CLONE_VM` and friends return `-ENOSYS` even though a real kernel honours
  them.** HYPERION cannot provide a shared *guest* address space: guest pages
  live in `std::vector` storage inside `GuestAddressSpace`. Silently forking
  instead would hand the guest a private copy it believes is shared, and it would
  corrupt its own memory in a way it cannot detect. A clean `ENOSYS` is the
  honest answer. Consequence: **multi-threaded guests do not run.**
* **`vfork` does not share the address space.** The parent *is* suspended until
  the child execs or exits, which is the part callers depend on; the child
  nevertheless gets a private memory copy. This is strictly safer than the
  kernel's behaviour, since a `vfork` child that writes to the parent's memory is
  undefined behaviour anyway.
* **Namespace flags return `-ENOSYS`** because creating a namespace requires
  `CAP_SYS_ADMIN`. Unrooted Android/Termux does not have it, so these could never
  work on the primary target and are not attempted.
* **A forked child runs on Tier 0 by default.** The parent keeps whichever tier it
  was using. Tier 0 is the reference interpreter that the differential test
  validates Tier 1 against, so child execution is the better-understood path.
* **Guest signal handlers are not invoked.** `rt_sigaction`/`rt_sigprocmask` are
  accepted and recorded, and children are real host children so `SIGCHLD` does
  not block or kill the parent, but a handler the guest installed will not run.
  Guests that poll with `wait4` — BusyBox and glibc's `system()`/`posix_spawn`
  both do — work; guests that rely purely on a `SIGCHLD` handler do not.

### Verified end-to-end

Against a real statically linked x86-64 BusyBox (Debian `busybox-static`
1:1.37.0), whose `sh` uses `clone(CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD)`,
`wait4`, `dup2`, `pipe2`, `execve` and installs a `SIGCHLD` handler:

```
$ hyperion run --quiet busybox-x86_64 sh -c 'printf "9\n2\n5\n1\n" | sort -n'
1
2
5
9
$ hyperion run --quiet busybox-x86_64 sh -c 'echo benchmark | wc -c'
10
```

Both match the native output byte for byte, with exit status 0. Reproduce with
`ctest -R guest.process`; see TESTING.md.

### Why `/proc/self/exe` is intercepted

A guest runs *inside* the translator's host process, so the host's
`/proc/self/exe` names **hyperion**, not the guest image. BusyBox re-executes
itself through exactly that path to run an applet, so without redirection it
would `execve` the translator and its own arguments would be reinterpreted as
translator commands. `readlink("/proc/self/exe")` and `execve("/proc/self/exe")`
are therefore redirected to the guest's own image, and `AT_EXECFN` is filled with
that same absolute path — which also matters because glibc's `_dl_get_origin()`
aborts the process when `AT_EXECFN` and `readlink("/proc/self/exe")` disagree.

---

## 5.1 Program loading (ELF64) and process startup

| Feature | Status | Phase | Covering test |
| --- | --- | --- | --- |
| ELF64 header validation (class, data, version, machine, type) | ✅ | 6 | `elf.rejects_bad_magic`/`_32_bit_image`/`_big_endian_image`/`_a_non_x86_64_machine`/`_a_relocatable_object`/`_a_truncated_file` |
| Program-header table bounds | ✅ | 6 | `elf.rejects_a_program_header_table_past_eof` |
| `PT_LOAD` bounds (`p_filesz <= p_memsz`, bytes past EOF, `p_vaddr ≡ p_offset`) | ✅ | 6 | `elf.rejects_filesz_greater_than_memsz`/`_segment_bytes_past_eof`/`_an_unaligned_segment` |
| Segment mapping (permissions, BSS zero-fill, unaligned `vaddr`, page sharing) | ✅ | 6 | `elf.maps_a_static_image_and_sets_up_the_stack`, `address_space.map_segment_*` |
| Startup stack (argc/argv/envp, 16-byte-aligned `rsp`) | ✅ | 6 | `elf.maps_a_static_image_and_sets_up_the_stack` |
| `hyperion info <elf>` / `run <elf>` | ✅ | 6 | `cli.elf` |
| Hostile-input robustness (parse + map) | ✅ | 6 | `elf.survives_random_and_mutated_input` |
| Dynamic linking (`PT_INTERP`, shared objects, relocations) | ⬜ (refused explicitly) | 6 | `elf.detects_a_dynamic_interpreter` |
| `auxv` contents (`AT_*` entries) | ⬜ (only `AT_NULL` emitted) | 6 | — |
| Environment variables in the startup stack | ⬜ (empty env) | 6 | — |
| Non-`PT_LOAD` program headers (`PT_PHDR`, `PT_GNU_STACK`, …) | ⬜ (ignored) | 6 | — |

A **dynamically linked** executable is parsed (so `info` can print its
interpreter) but refused by the loader with `ElfError::kNeedsInterpreter`: there
is no userspace dynamic loader, and loading it anyway would fail somewhere
confusing. The startup `auxv` is currently a single `AT_NULL` pair, so a program
that reads `AT_PAGESZ`/`AT_HWCAP`/… sees an empty vector — a documented gap, not
a wrong value.

---

## 6. Host forwarding

| Capability | Status | Phase |
| --- | --- | --- |
| Native library forwarding (allow-listed) | ⬜ | 9 |
| Graphics API forwarding (optional) | ⬜ | 9 |

---

## 7. How to update this file

When implementing a feature: set its status, add the covering test name, and
commit the change in the same milestone. When discovering an unsupported
instruction during testing: add a ❌ row and a `docs/known-issues.md` entry.
