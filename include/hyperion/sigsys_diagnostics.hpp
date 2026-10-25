// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// What to print when the platform kills the process with SIGSYS.
//
// Android builds its app seccomp filter with SECCOMP_RET_TRAP, so a syscall that
// is not on the device's allowlist raises SIGSYS and the process dies. The
// default disposition prints nothing: `hyperion run prog` would simply vanish
// with exit status 159 and no explanation, which is how a stray statx(2) on
// Android 10 looked before it was fixed (see hyperion/host_syscalls.hpp).
//
// Two policies, and the difference between them is deliberately explicit:
//
//   SigsysPolicy::kReportAndDie (the default)
//       Print, then restore the default disposition and re-raise. The process
//       still terminates from SIGSYS, so every caller that inspects the exit
//       status or the CTest verdict sees exactly what it saw before - only with
//       a reason attached. Used by the test harness: a trapped syscall during a
//       test is a failure that must stay loud and must not be retried.
//
//   SigsysPolicy::kAnswerEnosys
//       Print, then write -ENOSYS into the return-value register of the
//       interrupted context and return. The kernel rolls the syscall back
//       before delivering SIGSYS (the return register holds the syscall number,
//       not the first argument, and nothing was executed), so this makes the
//       caller see the ordinary "this kernel has no such syscall" result:
//       `syscall()` returns -1 with errno == ENOSYS, and the fallbacks that
//       already exist for ENOSYS run normally. Used by the CLI, so a future
//       trapped syscall degrades one operation instead of killing `hyperion run`
//       and the guest with it.
//
// That second policy is the same technique Android's own bionic uses for
// heapprofd (`HandleSigsysSeccompOverride`) and the one Bun adopted for Android:
// a seccomp trap is the *only* signal a process gets, and turning it into ENOSYS
// is the only way to keep running. A SIGSYS that was not sent by seccomp
// (`si_code != SYS_SECCOMP`, i.e. somebody used kill(2)) is never converted: it
// is re-raised under the previous disposition, because that is a real signal
// somebody asked for.
//
// Neither policy invents success. A converted call reports "not implemented on
// this device", which is true, and the conversion is printed once per syscall
// number so it cannot pass unnoticed.
#ifndef HYPERION_SIGSYS_DIAGNOSTICS_HPP
#define HYPERION_SIGSYS_DIAGNOSTICS_HPP

namespace hyperion {

enum class SigsysPolicy {
    kReportAndDie,
    kAnswerEnosys,
};

// Install the handler. Safe to call once at start-up; calling it twice is
// harmless. `program_name` must outlive the process (a string literal is ideal).
void install_sigsys_diagnostics(const char* program_name,
                                SigsysPolicy policy = SigsysPolicy::kReportAndDie);

// How many seccomp traps this process has converted to -ENOSYS so far. Always
// zero under kReportAndDie, since the process does not survive a trap there.
unsigned long sigsys_traps_converted();

// Name the operation currently in progress, so a report can say what the
// process was doing when the signal arrived. The pointer must stay valid; pass
// nullptr to clear it. The test harness sets this per test, the CLI leaves it
// unset (a guest instruction boundary is not known here).
void set_sigsys_context(const char* context);

} // namespace hyperion

#endif // HYPERION_SIGSYS_DIAGNOSTICS_HPP
