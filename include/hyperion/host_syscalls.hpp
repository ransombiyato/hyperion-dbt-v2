// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Which *host* syscalls HYPERION is allowed to issue.
//
// Why this is not a micro-optimisation
// -----------------------------------
// Android installs a seccomp-BPF filter on every app process, Termux included.
// Unlike a normal seccomp filter it does not answer a disallowed syscall with
// EPERM or ENOSYS: the policy is built with SECCOMP_RET_TRAP, so the kernel
// delivers SIGSYS - "Bad system call", exit status 159 - and the process dies
// on the spot. There is no way to catch a missing errno and fall back, and the
// guest cannot be told what happened because the translator itself is gone.
//
// The allowlist is derived from that Android release's bionic syscall list, so
// it is frozen at the device's API level. Two syscalls HYPERION needs are only
// present from Android 11 (API 30):
//
//   statx         - in bionic's SYSCALLS.TXT from android11-release onwards
//   memfd_create  - likewise
//
// On Android 10 (API 29, the primary target of this project) both are outside
// the allowlist and both are fatal. HYPERION therefore takes a portable path on
// those devices:
//
//   statx        -> synthesized from fstatat(2)/fstat(2), which are allowed
//   memfd_create -> O_TMPFILE fallback in a writable temporary directory, which
//                   is allowed, or a clean -ENOSYS if that is not available
//
// The portable paths are also force-usable on any Linux host with
// HYPERION_PORTABLE_SYSCALLS=1, which is how the test suite exercises them on
// x86-64 CI (see tests/unit/test_host_syscalls.cpp).
#ifndef HYPERION_HOST_SYSCALLS_HPP
#define HYPERION_HOST_SYSCALLS_HPP

namespace hyperion {

// True when the host is known to permit the raw statx(2) syscall. False on
// Android older than API 30, and false when the portable paths are forced.
bool host_statx_syscall_usable();

// True when the host is known to permit memfd_create(2). Same rules.
bool host_memfd_create_syscall_usable();

// The device's Android API level, or 0 when this is not Android or the value
// could not be read. Exposed so `hyperion probe` and the tests can report why a
// portable path is in use.
int host_android_api_level();

// True when HYPERION_PORTABLE_SYSCALLS is set, i.e. the portable paths were
// requested explicitly. Reported by `hyperion probe`.
bool host_portable_syscalls_forced();

} // namespace hyperion

#endif // HYPERION_HOST_SYSCALLS_HPP
