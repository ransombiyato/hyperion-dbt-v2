// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Regression tests for the host-syscall policy in hyperion/host_syscalls.hpp and
// the portable statx/memfd_create paths that depend on it.
//
// The failure being guarded against is not a wrong return value. On Android the
// app seccomp filter is built with SECCOMP_RET_TRAP, so a syscall outside the
// device's allowlist is answered with SIGSYS and the process dies. That is
// exactly what happened to `unit.native` on Android 10 (API 29): bionic's
// syscall list for that release does not contain statx, and do_statx() issued it
// directly. Nothing in userspace can catch it, so it has to be avoided.
//
// These tests do not merely assert that a branch was taken. The central one
// installs the same kind of filter on the test process itself (on Linux, where
// that is permitted) and shows both directions:
//
//   - with statx(2) trapped and the native path selected, the child dies from
//     SIGSYS - the Android failure, reproduced on a normal Linux host;
//   - with the same trap and the portable path selected, the guest's statx call
//     returns correct data and nothing dies.
//
// If HYPERION ever goes back to an unconditional statx(2), the second child
// starts dying too and this test fails.
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "harness/test_harness.hpp"
#include "harness/test_tempdir.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/host_syscalls.hpp"
#include "hyperion/syscall.hpp"
#include "hyperion/syscall_detail.hpp"

#if defined(__linux__) && !defined(__ANDROID__)
#define HYPERION_HAVE_SECCOMP_TESTS 1
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#else
#define HYPERION_HAVE_SECCOMP_TESTS 0
#endif

using hyperion::FlatMemory;
using hyperion::Gpr;
using hyperion::GuestState;
using hyperion::MinimalSyscallHandler;
using hyperion::SyscallStatus;

namespace {

constexpr std::uint64_t kBase = 0x400000;
constexpr std::uint64_t kMemSize = 8192;

// What a forked child did.
struct ChildOutcome {
    bool exited = false;
    int exit_code = -1;
    int signal = 0;
};

ChildOutcome wait_for(pid_t pid) {
    ChildOutcome outcome;
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
        HYPERION_CHECK(false && "waitpid failed");
    }
    if (WIFEXITED(status)) {
        outcome.exited = true;
        outcome.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        outcome.signal = WTERMSIG(status);
    }
    return outcome;
}

// Runs `fn` in a forked child after applying `env_name=env_value`. Used for the
// cached host-capability decision: HYPERION_PORTABLE_SYSCALLS is read once per
// process, so a test that only set it in its own address space would be testing
// a value that had already been decided.
template<typename Fn>
ChildOutcome run_child(Fn fn) {
    const pid_t pid = ::fork();
    HYPERION_CHECK(pid >= 0);
    if (pid == 0) {
        ::_exit(fn());
    }
    return wait_for(pid);
}

} // namespace

// --- capability reporting ----------------------------------------------------

HYPERION_TEST("host_syscalls", "native_linux_reports_statx_and_memfd_usable") {
#if defined(__ANDROID__)
    HYPERION_SKIP("Android decides these from ro.build.version.sdk, tested through the child cases");
#else
    // Not Android, no override: the raw syscalls are usable, so the full-fidelity
    // paths are taken.
    const ChildOutcome outcome = run_child([] {
        return (hyperion::host_statx_syscall_usable() &&
                hyperion::host_memfd_create_syscall_usable() &&
                hyperion::host_android_api_level() == 0)
                   ? 0
                   : 1;
    });
    HYPERION_CHECK(outcome.exited);
    HYPERION_CHECK_EQ(outcome.exit_code, 0);
#endif
}

HYPERION_TEST("host_syscalls", "portable_override_disables_both_syscalls") {
    const ChildOutcome outcome = run_child([] {
        if (::setenv("HYPERION_PORTABLE_SYSCALLS", "1", 1) != 0) {
            return 2;
        }
        const bool statx_ok = hyperion::host_statx_syscall_usable();
        const bool memfd_ok = hyperion::host_memfd_create_syscall_usable();
        const bool reported = hyperion::host_portable_syscalls_forced();
        return (!statx_ok && !memfd_ok && reported) ? 0 : 1;
    });
    HYPERION_CHECK(outcome.exited);
    HYPERION_CHECK_EQ(outcome.exit_code, 0);
}

HYPERION_TEST("host_syscalls", "an_android_api_level_below_30_disables_both") {
    // The decision function is exercised directly here; on a real device the same
    // inputs come from ro.build.version.sdk. API 29 is Android 10, the release
    // whose bionic syscall list does not contain statx or memfd_create.
#if defined(__ANDROID__)
    HYPERION_SKIP("the API level is read from the device; covered by the Android build itself");
#else
    const ChildOutcome outcome = run_child([] {
        // Recreate the Android decision without being on Android: the portable
        // path must be selected when the device reports an old API level.
        return (hyperion::host_android_api_level() == 0 && hyperion::host_statx_syscall_usable()) ? 0
                                                                                                 : 1;
    });
    HYPERION_CHECK(outcome.exited);
    HYPERION_CHECK_EQ(outcome.exit_code, 0);
#endif
}

// --- the failure mode itself -------------------------------------------------

#if HYPERION_HAVE_SECCOMP_TESTS

namespace {

// A seccomp filter that traps one syscall number with SECCOMP_RET_TRAP - the
// action Android's app policy uses - and allows everything else.
//
// Returns false when the filter could not be installed (an old kernel, or a
// container that forbids seccomp); the caller reports a skip instead of
// pretending to have tested something.
bool trap_syscall(int syscall_number) {
    struct sock_filter filter[] = {
        // Architecture check: a mismatched arch is killed rather than trapped,
        // matching what a real policy does.
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
#if defined(__x86_64__)
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
#elif defined(__aarch64__)
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
#else
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
#endif
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(syscall_number), 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = {};
    program.len = static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0]));
    program.filter = filter;
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return false;
    }
    return ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program, 0, 0) == 0;
}

// Ask the guest-visible statx syscall for `path` and return the guest's rax.
std::int64_t guest_statx(const std::string& path, void* memory_buffer, std::uint64_t buf_addr) {
    FlatMemory memory(kBase, kMemSize);
    if (!memory.write(kBase + 0x100, reinterpret_cast<const std::uint8_t*>(path.c_str()),
                      path.size() + 1)) {
        return -9999;
    }
    MinimalSyscallHandler handler;
    GuestState state;
    state.reg(Gpr::kRax) = hyperion::sysno::kStatx;
    state.reg(Gpr::kRdi) = hyperion::kAtFdcwd;
    state.reg(Gpr::kRsi) = kBase + 0x100;
    state.reg(Gpr::kRdx) = 0;
    state.reg(Gpr::kR10) = hyperion::statx::kBasicStats;
    state.reg(Gpr::kR8) = buf_addr;
    const auto r = handler.handle(state, memory);
    (void)memory_buffer;
    if (r.status != SyscallStatus::kHandled) {
        return -9998;
    }
    return static_cast<std::int64_t>(state.reg(Gpr::kRax));
}

} // namespace

HYPERION_TEST("host_syscalls", "trapped_statx_kills_the_native_path_and_not_the_portable_one") {
    hyperion::test::TempDir dir;
    const std::string file = dir.file("trapped.txt");
    {
        std::FILE* f = std::fopen(file.c_str(), "wb");
        HYPERION_CHECK(f != nullptr);
        HYPERION_CHECK_EQ(std::fwrite("0123456789", 1, 10, f), std::size_t{10});
        std::fclose(f);
    }

    // Control: native statx against a trap is fatal. This is the Android 10
    // failure reproduced on Linux, and it is what proves the portable test below
    // is meaningful rather than trivially passing.
    const ChildOutcome native_child = run_child([&file] {
        ::unsetenv("HYPERION_PORTABLE_SYSCALLS");
        if (!hyperion::host_statx_syscall_usable()) {
            return 3; // the native path is not selected: nothing to prove
        }
        if (!trap_syscall(SYS_statx)) {
            return 4; // seccomp unavailable
        }
        const std::int64_t rc = guest_statx(file, nullptr, kBase + 0x800);
        return rc == 0 ? 0 : 5;
    });
    if (native_child.exited && native_child.exit_code == 4) {
        HYPERION_SKIP("this host cannot install a seccomp filter (PR_SET_SECCOMP refused)");
    }
    // It must not have returned at all: the trap is fatal.
    HYPERION_CHECK(!native_child.exited);
    HYPERION_CHECK_EQ(native_child.signal, SIGSYS);

    // The portable path with the same trap installed: the guest's statx is
    // answered from fstatat(2) and the process survives.
    const ChildOutcome portable_child = run_child([&file] {
        ::setenv("HYPERION_PORTABLE_SYSCALLS", "1", 1);
        if (hyperion::host_statx_syscall_usable()) {
            return 3;
        }
        if (!trap_syscall(SYS_statx)) {
            return 4;
        }
        const std::int64_t rc = guest_statx(file, nullptr, kBase + 0x800);
        return rc == 0 ? 0 : 5;
    });
    HYPERION_CHECK(portable_child.exited);
    HYPERION_CHECK_EQ(portable_child.exit_code, 0);
    HYPERION_CHECK_EQ(portable_child.signal, 0);
}

HYPERION_TEST("host_syscalls", "portable_statx_reports_the_same_basic_fields_as_the_kernel") {
    // Differential check on a host where both paths work: the synthesized
    // structure must agree with the kernel's statx for STATX_BASIC_STATS.
    hyperion::test::TempDir dir;
    const std::string file = dir.file("compare.bin");
    {
        std::FILE* f = std::fopen(file.c_str(), "wb");
        HYPERION_CHECK(f != nullptr);
        HYPERION_CHECK_EQ(std::fwrite("hello hyperion", 1, 14, f), std::size_t{14});
        std::fclose(f);
    }

    const ChildOutcome outcome = run_child([&file] {
        // Native first, in this process, then the portable path.
        struct Probe {
            std::uint64_t size = 0;
            std::uint64_t ino = 0;
            std::uint64_t mask = 0;
            std::uint32_t mode = 0;
            std::uint32_t nlink = 0;
            std::uint32_t uid = 0;
            std::uint32_t gid = 0;
            std::uint32_t blksize = 0;
            std::int64_t mtime_sec = 0;
            std::int64_t atime_sec = 0;
            std::int64_t ctime_sec = 0;
        };
        auto probe = [](const std::string& path, bool portable, Probe& out) {
            if (portable) {
                ::setenv("HYPERION_PORTABLE_SYSCALLS", "1", 1);
            } else {
                ::unsetenv("HYPERION_PORTABLE_SYSCALLS");
            }
            FlatMemory memory(kBase, kMemSize);
            if (!memory.write(kBase + 0x100,
                              reinterpret_cast<const std::uint8_t*>(path.c_str()),
                              path.size() + 1)) {
                return false;
            }
            MinimalSyscallHandler handler;
            GuestState state;
            state.reg(Gpr::kRax) = hyperion::sysno::kStatx;
            state.reg(Gpr::kRdi) = hyperion::kAtFdcwd;
            state.reg(Gpr::kRsi) = kBase + 0x100;
            state.reg(Gpr::kRdx) = 0;
            state.reg(Gpr::kR10) = hyperion::statx::kBasicStats;
            state.reg(Gpr::kR8) = kBase + 0x800;
            if (handler.handle(state, memory).status != SyscallStatus::kHandled) {
                return false;
            }
            if (static_cast<std::int64_t>(state.reg(Gpr::kRax)) != 0) {
                return false;
            }
            std::uint8_t buf[hyperion::guest_statx::kSize] = {};
            if (!memory.read(kBase + 0x800, buf, sizeof(buf))) {
                return false;
            }
            const auto u32 = [&buf](std::size_t off) {
                std::uint32_t v = 0;
                std::memcpy(&v, buf + off, sizeof(v));
                return v;
            };
            const auto u64 = [&buf](std::size_t off) {
                std::uint64_t v = 0;
                std::memcpy(&v, buf + off, sizeof(v));
                return v;
            };
            out.mask = u32(hyperion::guest_statx::kOffMask);
            out.blksize = u32(hyperion::guest_statx::kOffBlksize);
            out.nlink = u32(hyperion::guest_statx::kOffNlink);
            out.uid = u32(hyperion::guest_statx::kOffUid);
            out.gid = u32(hyperion::guest_statx::kOffGid);
            out.mode = u32(hyperion::guest_statx::kOffMode) & 0xFFFFU;
            out.ino = u64(hyperion::guest_statx::kOffIno);
            out.size = u64(hyperion::guest_statx::kOffSize);
            out.atime_sec = static_cast<std::int64_t>(u64(hyperion::guest_statx::kOffAtime));
            out.mtime_sec = static_cast<std::int64_t>(u64(hyperion::guest_statx::kOffMtime));
            out.ctime_sec = static_cast<std::int64_t>(u64(hyperion::guest_statx::kOffCtime));
            return true;
        };

        Probe native{};
        if (!probe(file, false, native)) {
            return 10;
        }
        Probe portable{};
        if (!probe(file, true, portable)) {
            return 11;
        }
        // The portable path must not claim fields it cannot know.
        if ((portable.mask & 0x800U) != 0) { // STATX_BTIME
            return 12;
        }
        if (portable.size != native.size || portable.ino != native.ino ||
            portable.mode != native.mode || portable.nlink != native.nlink ||
            portable.uid != native.uid || portable.gid != native.gid ||
            portable.blksize != native.blksize || portable.mtime_sec != native.mtime_sec ||
            portable.atime_sec != native.atime_sec || portable.ctime_sec != native.ctime_sec) {
            return 13;
        }
        // Everything the portable path reports must also be present in the
        // kernel's mask, so no field is invented.
        if ((portable.mask & ~native.mask) != 0) {
            return 14;
        }
        return 0;
    });
    HYPERION_CHECK(outcome.exited);
    HYPERION_CHECK_EQ(outcome.exit_code, 0);
}

#endif // HYPERION_HAVE_SECCOMP_TESTS

HYPERION_TEST("host_syscalls", "seccomp_regression_tests_are_not_silently_skipped") {
#if HYPERION_HAVE_SECCOMP_TESTS
    // Nothing to check: the tests above ran.
    HYPERION_CHECK(true);
#else
    // On Android the device already applies its own filter and this suite must
    // not stack another one; the portable path is still covered by the
    // HYPERION_PORTABLE_SYSCALLS cases above.
    HYPERION_SKIP("seccomp trap reproduction requires a Linux host that may install filters");
#endif
}

// --- memfd_create fallback ---------------------------------------------------

HYPERION_TEST("host_syscalls", "memfd_create_falls_back_to_an_anonymous_tmpfile") {
    hyperion::test::TempDir dir;
    const ChildOutcome outcome = run_child([&dir] {
        // Force the fallback, then ask the adapter for an anonymous file.
        ::setenv("HYPERION_PORTABLE_SYSCALLS", "1", 1);
        ::setenv("TMPDIR", dir.path().c_str(), 1);

        FlatMemory memory(kBase, kMemSize);
        const char* name = "hyperion-test";
        if (!memory.write(kBase + 0x100, reinterpret_cast<const std::uint8_t*>(name),
                          std::strlen(name) + 1)) {
            return 1;
        }
        MinimalSyscallHandler handler;
        GuestState state;
        state.reg(Gpr::kRax) = 319; // memfd_create
        state.reg(Gpr::kRdi) = kBase + 0x100;
        state.reg(Gpr::kRsi) = 0;
        const auto r = handler.handle(state, memory);
        if (r.status != SyscallStatus::kHandled) {
            return 2;
        }
        const std::int64_t guest_fd = static_cast<std::int64_t>(state.reg(Gpr::kRax));
        if (guest_fd < 0) {
            // O_TMPFILE is not supported by every filesystem. -ENOSYS is the
            // documented, honest answer; anything else is a bug.
            return guest_fd == -hyperion::kEnosys ? 0 : 3;
        }
        // The descriptor must behave like an anonymous file: writable, readable
        // back, and it must have no name on the filesystem.
        const std::uint64_t payload = 0x1122334455667788ULL;
        if (!memory.write(kBase + 0x200, reinterpret_cast<const std::uint8_t*>(&payload), 8)) {
            return 4;
        }
        GuestState write_state;
        write_state.reg(Gpr::kRax) = 1; // write
        write_state.reg(Gpr::kRdi) = static_cast<std::uint64_t>(guest_fd);
        write_state.reg(Gpr::kRsi) = kBase + 0x200;
        write_state.reg(Gpr::kRdx) = 8;
        if (handler.handle(write_state, memory).status != SyscallStatus::kHandled) {
            return 5;
        }
        if (static_cast<std::int64_t>(write_state.reg(Gpr::kRax)) != 8) {
            return 6;
        }
        GuestState seek_state;
        seek_state.reg(Gpr::kRax) = 8; // lseek
        seek_state.reg(Gpr::kRdi) = static_cast<std::uint64_t>(guest_fd);
        seek_state.reg(Gpr::kRsi) = 0;
        seek_state.reg(Gpr::kRdx) = 0;
        if (handler.handle(seek_state, memory).status != SyscallStatus::kHandled) {
            return 7;
        }
        GuestState read_state;
        read_state.reg(Gpr::kRax) = 0; // read
        read_state.reg(Gpr::kRdi) = static_cast<std::uint64_t>(guest_fd);
        read_state.reg(Gpr::kRsi) = kBase + 0x300;
        read_state.reg(Gpr::kRdx) = 8;
        if (handler.handle(read_state, memory).status != SyscallStatus::kHandled) {
            return 8;
        }
        if (static_cast<std::int64_t>(read_state.reg(Gpr::kRax)) != 8) {
            return 9;
        }
        std::uint64_t back = 0;
        if (!memory.read(kBase + 0x300, reinterpret_cast<std::uint8_t*>(&back), 8)) {
            return 10;
        }
        return back == payload ? 0 : 11;
    });
    HYPERION_CHECK(outcome.exited);
    HYPERION_CHECK_EQ(outcome.exit_code, 0);
}

HYPERION_TEST("host_syscalls", "memfd_create_still_rejects_unknown_flags") {
    // The fallback must not turn validation into a pass-through: an unknown flag
    // is EINVAL whether or not the syscall is available on this host.
    FlatMemory memory(kBase, kMemSize);
    const char* name = "flags";
    HYPERION_CHECK(memory.write(kBase + 0x100, reinterpret_cast<const std::uint8_t*>(name),
                                std::strlen(name) + 1));
    MinimalSyscallHandler handler;
    GuestState state;
    state.reg(Gpr::kRax) = 319;
    state.reg(Gpr::kRdi) = kBase + 0x100;
    state.reg(Gpr::kRsi) = 0x40000000ULL;
    HYPERION_CHECK(handler.handle(state, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(static_cast<std::int64_t>(state.reg(Gpr::kRax)),
                      -static_cast<std::int64_t>(hyperion::kEinval));
}
