// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
// REG_RAX and friends are exposed by glibc only under _GNU_SOURCE; this must
// come before every include in the translation unit.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "hyperion/sigsys_diagnostics.hpp"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ucontext.h>

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
SigsysPolicy g_policy = SigsysPolicy::kReportAndDie;
unsigned long g_converted = 0;

// Syscall numbers already reported under kAnswerEnosys. Reporting a conversion
// once per number keeps the log useful (a guest polling a missing syscall cannot
// flood it) without hiding that more than one kind of call was affected. Fixed
// size and no allocation: this runs in a signal handler.
constexpr std::size_t kMaxReported = 8;
long g_reported[kMaxReported] = {};
unsigned long g_reported_count = 0;

bool already_reported(long number) {
    for (unsigned long i = 0; i < g_reported_count && i < kMaxReported; ++i) {
        if (g_reported[i] == number) {
            return true;
        }
    }
    return false;
}

void remember_reported(long number) {
    if (g_reported_count < kMaxReported) {
        g_reported[g_reported_count++] = number;
    }
}

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

void write_cstr(const char* text) {
    write_stderr(text, std::strlen(text));
}

// Put -ENOSYS where the interrupted `syscall`/`svc` instruction will look for its
// result. The kernel has already rolled the syscall back, so that register holds
// the syscall number and nothing has been executed.
//
// Returns false on an architecture this file does not know how to patch, in
// which case the caller must not return: it falls back to dying from SIGSYS,
// which is at least honest.
bool write_enosys_return(void* context) {
#if defined(__linux__)
    if (context == nullptr) {
        return false;
    }
    auto* const uc = static_cast<ucontext_t*>(context); // NOLINT
#if defined(__x86_64__) && !defined(__ILP32__)
    uc->uc_mcontext.gregs[REG_RAX] = static_cast<greg_t>(-ENOSYS);
    return true;
#elif defined(__aarch64__)
    // glibc calls this gregset_t, bionic calls it regs; both are the kernel's
    // struct sigcontext and both put x0 at index 0.
    uc->uc_mcontext.regs[0] =
        static_cast<unsigned long long>(static_cast<long long>(-static_cast<long long>(ENOSYS)));
    return true;
#elif defined(__arm__)
    uc->uc_mcontext.arm_r0 = static_cast<unsigned long>(static_cast<std::uint32_t>(-ENOSYS));
    return true;
#else
    (void)uc;
    return false;
#endif
#else
    (void)context;
    return false;
#endif
}

// Async-signal-safe: write(2) and snprintf over stack buffers only. stdio is
// deliberately avoided - a signal can arrive while stdio holds its lock.
extern "C" void report_sigsys(int signal_number, siginfo_t* info, void* ucontext_ptr) {
    const bool seccomp_trap = info != nullptr && info->si_code == SYS_SECCOMP;

    // A seccomp trap, and this process is allowed to keep running: answer the
    // call with -ENOSYS and resume. Everything that already handles ENOSYS then
    // takes over; nothing is claimed to have succeeded.
    if (seccomp_trap && g_policy == SigsysPolicy::kAnswerEnosys) {
        const long number = static_cast<long>(info->si_syscall);
        if (!already_reported(number)) {
            remember_reported(number);
            char note[256];
            const int n = std::snprintf(
                note, sizeof(note),
                "%s: seccomp refused %s(%ld); answering -ENOSYS and continuing\n"
                "  (one line per syscall number; see include/hyperion/host_syscalls.hpp)\n",
                g_program_name, syscall_name(number), number);
            if (n > 0) {
                const auto length = static_cast<std::size_t>(n);
                write_stderr(note, length < sizeof(note) ? length : sizeof(note) - 1);
            }
        }
        if (write_enosys_return(ucontext_ptr)) {
            ++g_converted;
            return;
        }
        // No known register to patch on this architecture: fall through and die
        // from SIGSYS rather than resume with a value nobody can trust.
        write_cstr("  ...but this architecture's return register is unknown to\n"
                   "  sigsys_diagnostics.cpp, so the process cannot continue.\n");
    }

    char buffer[512];
    const char* const activity = g_context;
    const int len =
        std::snprintf(buffer, sizeof(buffer),
                      "\n%s: SIGSYS (bad system call)%s%s\n"
                      "  si_code=%d%s\n"
                      "  syscall=%ld (%s)\n",
                      g_program_name, activity != nullptr ? " while running " : "",
                      activity != nullptr ? activity : "", info != nullptr ? info->si_code : -1,
                      seccomp_trap ? " SYS_SECCOMP" : "",
                      (info != nullptr) ? static_cast<long>(info->si_syscall) : -1L,
                      (info != nullptr) ? syscall_name(static_cast<long>(info->si_syscall)) : "?");
    if (len > 0) {
        const auto length = static_cast<std::size_t>(len);
        write_stderr(buffer, length < sizeof(buffer) ? length : sizeof(buffer) - 1);
    }
    if (seccomp_trap) {
        write_cstr("  A seccomp filter refused that system call and killed the process.\n"
                   "  Android's app policy uses SECCOMP_RET_TRAP; see\n"
                   "  include/hyperion/host_syscalls.hpp and docs/known-issues.md.\n");
    }
    // The verdict is unchanged: die from SIGSYS, exactly as without the handler.
    (void)::signal(signal_number, SIG_DFL);
    (void)::raise(signal_number);
}

} // namespace

void install_sigsys_diagnostics(const char* program_name, SigsysPolicy policy) {
#if defined(__linux__)
    if (program_name != nullptr && *program_name != '\0') {
        g_program_name = program_name;
    }
    g_policy = policy;
    struct sigaction action = {};
    action.sa_sigaction = &report_sigsys;
    action.sa_flags = SA_SIGINFO;
    (void)::sigemptyset(&action.sa_mask);
    (void)::sigaction(SIGSYS, &action, nullptr);
#else
    (void)program_name;
    (void)policy;
#endif
}

unsigned long sigsys_traps_converted() {
    return g_converted;
}

void set_sigsys_context(const char* context) {
    g_context = context;
}

} // namespace hyperion
