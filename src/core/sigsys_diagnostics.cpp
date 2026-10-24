// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "hyperion/sigsys_diagnostics.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
// SYS_SECCOMP is the si_code a seccomp trap reports. glibc exposes it through
// <signal.h>; bionic needs the seccomp header.
#if defined(__ANDROID__)
#include <linux/seccomp.h>
#endif
#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif
#endif

namespace hyperion {
namespace {

const char* g_program_name = "hyperion";
const char* volatile g_context = nullptr;

// Only the syscalls that are known to be trapped by Android's app policy, plus
// the ones HYPERION itself issues. An unlisted number is still reported, just
// without a name.
const char* syscall_name(long number) {
#if defined(__linux__)
    switch (number) {
#if defined(SYS_statx)
    case SYS_statx:
        return "statx";
#endif
#if defined(SYS_memfd_create)
    case SYS_memfd_create:
        return "memfd_create";
#endif
#if defined(SYS_renameat2)
    case SYS_renameat2:
        return "renameat2";
#endif
#if defined(SYS_faccessat2)
    case SYS_faccessat2:
        return "faccessat2";
#endif
#if defined(SYS_openat2)
    case SYS_openat2:
        return "openat2";
#endif
#if defined(SYS_clone3)
    case SYS_clone3:
        return "clone3";
#endif
#if defined(SYS_close_range)
    case SYS_close_range:
        return "close_range";
#endif
#if defined(SYS_pidfd_open)
    case SYS_pidfd_open:
        return "pidfd_open";
#endif
#if defined(SYS_rseq)
    case SYS_rseq:
        return "rseq";
#endif
#if defined(SYS_io_uring_setup)
    case SYS_io_uring_setup:
        return "io_uring_setup";
#endif
    default:
        break;
    }
#else
    (void)number;
#endif
    return "unknown";
}

void write_stderr(const char* text, std::size_t length) {
    std::size_t written = 0;
    while (written < length) {
        const ssize_t n = ::write(STDERR_FILENO, text + written, length - written);
        if (n <= 0) {
            return;
        }
        written += static_cast<std::size_t>(n);
    }
}

void write_cstr(const char* text) { write_stderr(text, std::strlen(text)); }

// Async-signal-safe: write(2) and snprintf over stack buffers only. stdio is
// deliberately avoided - a signal can arrive while stdio holds its lock.
extern "C" void report_sigsys(int signal_number, siginfo_t* info, void* /*context*/) {
    char buffer[512];
    const char* const context = g_context;
    const int len = std::snprintf(
        buffer, sizeof(buffer),
        "\n%s: SIGSYS (bad system call)%s%s\n"
        "  si_code=%d%s\n"
        "  syscall=%ld (%s)\n",
        g_program_name, context != nullptr ? " while running " : "",
        context != nullptr ? context : "", info != nullptr ? info->si_code : -1,
        (info != nullptr && info->si_code == SYS_SECCOMP) ? " SYS_SECCOMP" : "",
        (info != nullptr) ? static_cast<long>(info->si_syscall) : -1L,
        (info != nullptr) ? syscall_name(static_cast<long>(info->si_syscall)) : "?");
    if (len > 0) {
        const auto length = static_cast<std::size_t>(len);
        write_stderr(buffer, length < sizeof(buffer) ? length : sizeof(buffer) - 1);
    }
    if (info != nullptr && info->si_code == SYS_SECCOMP) {
        write_cstr("  A seccomp filter refused that system call and killed the process.\n"
                   "  Android's app policy uses SECCOMP_RET_TRAP, which cannot be caught; see\n"
                   "  include/hyperion/host_syscalls.hpp and docs/known-issues.md.\n");
    }
    // The verdict is unchanged: die from SIGSYS, exactly as without the handler.
    (void)::signal(signal_number, SIG_DFL);
    (void)::raise(signal_number);
}

} // namespace

void install_sigsys_diagnostics(const char* program_name) {
#if defined(__linux__)
    if (program_name != nullptr && *program_name != '\0') {
        g_program_name = program_name;
    }
    struct sigaction action = {};
    action.sa_sigaction = &report_sigsys;
    action.sa_flags = SA_SIGINFO;
    (void)::sigemptyset(&action.sa_mask);
    (void)::sigaction(SIGSYS, &action, nullptr);
#else
    (void)program_name;
#endif
}

void set_sigsys_context(const char* context) { g_context = context; }

} // namespace hyperion
