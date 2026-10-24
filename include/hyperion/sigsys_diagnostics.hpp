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
// These diagnostics do not change the outcome. They print which syscall was
// refused, then restore the default disposition and re-raise, so the process
// still terminates from SIGSYS and every caller that inspects the exit status or
// the CTest verdict sees exactly what it saw before - only with a reason
// attached. Nothing is caught, retried or converted into a fake success.
#ifndef HYPERION_SIGSYS_DIAGNOSTICS_HPP
#define HYPERION_SIGSYS_DIAGNOSTICS_HPP

namespace hyperion {

// Install the reporter. Safe to call once at start-up; calling it twice is
// harmless. `program_name` must outlive the process (a string literal is ideal).
void install_sigsys_diagnostics(const char* program_name);

// Name the operation currently in progress, so a report can say what the
// process was doing when the signal arrived. The pointer must stay valid; pass
// nullptr to clear it. The test harness sets this per test, the CLI leaves it
// unset (a guest instruction boundary is not known here).
void set_sigsys_context(const char* context);

} // namespace hyperion

#endif // HYPERION_SIGSYS_DIAGNOSTICS_HPP
