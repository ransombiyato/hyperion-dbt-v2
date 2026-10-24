// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Compatibility adapters: compiled-in implementations of guest syscalls that the
// existing handler does not implement.
//
// Every adapter here is written against the *guest's* x86-64 ABI and the host's
// AArch64/x86-64 ABI separately, and translates between them field by field. The
// transformations that matter are:
//
//   * guest descriptor numbers -> host descriptor numbers (the two spaces do not
//     coincide: a guest fd is an index into the handler's table);
//   * structure layouts that differ (struct epoll_event is 12 packed bytes on
//     x86-64 and 16 bytes on AArch64) and structures that are identical but are
//     still read/written through validated guest memory rather than by pointer;
//   * flag words whose values differ between the ABIs, and flag words that are
//     accepted only after an explicit whitelist check;
//   * the return convention: a non-negative value or a negative errno in rax.
//
// An adapter must never return -1 with a host errno, never hand a guest pointer
// to the kernel, and never succeed for a request it did not perform. When an
// adapter cannot honour a request it returns -ENOSYS, which the dispatcher
// counts as an unsupported syscall rather than a silent success.
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/times.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "hyperion/host_syscalls.hpp"
#include "hyperion/syscall_detail.hpp"
#include "hyperion/syscall_dispatch.hpp"

namespace hyperion {

namespace {

// Guest-visible struct sizes. `struct epoll_event` is the one layout that
// genuinely differs; the rest are listed so a future ABI change is caught by a
// test rather than by a silently corrupted field.
constexpr std::size_t kGuestEpollEventSize = 12; // x86-64: packed {u32 events; u64 data}
constexpr std::size_t kGuestEpollEventDataOffset = 4;
constexpr std::size_t kGuestPollfdSize = 8;  // {int fd; short events; short revents}
constexpr std::size_t kGuestFdSetSize = 128; // 1024 bits
constexpr int kGuestFdSetMax = 1024;
constexpr std::size_t kGuestRusageSize = 144;
constexpr std::size_t kGuestItimervalSize = 32;
constexpr std::size_t kGuestTmsSize = 32;
constexpr std::size_t kGuestSchedParamSize = 4;
constexpr std::size_t kGuestTimespecSize = 16;
constexpr std::size_t kGuestTimevalSize = 16;

std::int64_t errno_result() {
    return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
}

std::uint32_t get_u32_le(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    }
    return v;
}

constexpr const char* kTransformPathSysroot = "path.sysroot";
constexpr const char* kTransformGuestFd = "guest_fd";
constexpr const char* kTransformGuestBuffer = "guest_buffer";
constexpr const char* kTransformEpollEvent = "layout.epoll_event";
constexpr const char* kTransformTimespec = "layout.timespec";
constexpr const char* kTransformItimerspec = "layout.itimerspec";
constexpr const char* kTransformItimerval = "layout.itimerval";
constexpr const char* kTransformRusage = "layout.rusage";
constexpr const char* kTransformPollfd = "layout.pollfd";
constexpr const char* kTransformFdSet = "layout.fd_set";
constexpr const char* kTransformSchedParam = "layout.sched_param";
constexpr const char* kTransformTms = "layout.tms";
constexpr const char* kTransformSigmask = "sigmask.u64";

// Transform lists named by adapter descriptors. Each array is null-terminated so
// validation can compare a rule's declaration with what the code really does.
constexpr const char* kPathOnly[] = {kTransformPathSysroot, nullptr};
constexpr const char* kPathAndBuffer[] = {kTransformPathSysroot, kTransformGuestBuffer, nullptr};
constexpr const char* kFdOnly[] = {kTransformGuestFd, nullptr};
constexpr const char* kFdAndBuffer[] = {kTransformGuestFd, kTransformGuestBuffer, nullptr};
constexpr const char* kEpollCtl[] = {kTransformGuestFd, kTransformEpollEvent, nullptr};
constexpr const char* kEpollWait[] = {kTransformGuestFd, kTransformEpollEvent, nullptr};
constexpr const char* kEpollWaitMasked[] = {kTransformGuestFd, kTransformEpollEvent,
                                            kTransformSigmask, nullptr};
constexpr const char* kPollWait[] = {kTransformGuestFd, kTransformPollfd, kTransformSigmask,
                                     nullptr};
constexpr const char* kSelectWait[] = {kTransformGuestFd, kTransformFdSet, kTransformTimespec,
                                       kTransformSigmask, nullptr};
constexpr const char* kItimer[] = {kTransformItimerval, nullptr};
constexpr const char* kRusageList[] = {kTransformRusage, nullptr};
constexpr const char* kSchedParamList[] = {kTransformSchedParam, nullptr};
constexpr const char* kTimesList[] = {kTransformTms, nullptr};
constexpr const char* kTimerfdSet[] = {kTransformGuestFd, kTransformItimerspec, nullptr};

// --- files and directories ---------------------------------------------------

std::int64_t adapter_chdir(SyscallContext& ctx) {
    std::string path;
    const std::int64_t rc = ctx.read_cstr(ctx.arg(0), path);
    if (rc != 0) {
        return rc;
    }
    if (::chdir(ctx.resolve_path(path).c_str()) != 0) {
        return errno_result();
    }
    return 0;
}

std::int64_t adapter_fchdir(SyscallContext& ctx) {
    const std::int64_t host = ctx.host_fd(ctx.arg_i32(0));
    if (host < 0) {
        return host;
    }
    if (::fchdir(static_cast<int>(host)) != 0) {
        return errno_result();
    }
    return 0;
}

std::int64_t adapter_umask(SyscallContext& ctx) {
    const mode_t mask = static_cast<mode_t>(ctx.arg(0) & 0777ULL);
    return static_cast<std::int64_t>(::umask(mask));
}

std::int64_t adapter_truncate(SyscallContext& ctx) {
    std::string path;
    const std::int64_t rc = ctx.read_cstr(ctx.arg(0), path);
    if (rc != 0) {
        return rc;
    }
    const auto length = static_cast<off_t>(static_cast<std::int64_t>(ctx.arg(1)));
    if (length < 0) {
        return -kEinval;
    }
    if (::truncate(ctx.resolve_path(path).c_str(), length) != 0) {
        return errno_result();
    }
    return 0;
}

std::int64_t adapter_ftruncate(SyscallContext& ctx) {
    const std::int64_t host = ctx.host_fd(ctx.arg_i32(0));
    if (host < 0) {
        return host;
    }
    const auto length = static_cast<off_t>(static_cast<std::int64_t>(ctx.arg(1)));
    if (length < 0) {
        return -kEinval;
    }
    if (::ftruncate(static_cast<int>(host), length) != 0) {
        return errno_result();
    }
    return 0;
}

// symlinkat(target, newdirfd, linkpath). The *contents* of the link are not a
// path to resolve, so they are not sysroot-prefixed; only `linkpath` is.
std::int64_t adapter_symlinkat(SyscallContext& ctx) {
    std::string target;
    std::int64_t rc = ctx.read_cstr(ctx.arg(0), target);
    if (rc != 0) {
        return rc;
    }
    const std::int64_t dirfd = ctx.host_dirfd(ctx.arg_i32(1));
    if (dirfd < 0) {
        return dirfd;
    }
    std::string linkpath;
    rc = ctx.read_cstr(ctx.arg(2), linkpath);
    if (rc != 0) {
        return rc;
    }
    if (::symlinkat(target.c_str(), static_cast<int>(dirfd), ctx.resolve_path(linkpath).c_str()) !=
        0) {
        return errno_result();
    }
    return 0;
}

// linkat(olddirfd, oldpath, newdirfd, newpath, flags). AT_SYMLINK_FOLLOW is the
// only flag the kernel accepts; EMPTY_PATH is x86-64-only and is rejected.
std::int64_t adapter_linkat(SyscallContext& ctx) {
    const std::int64_t old_dirfd = ctx.host_dirfd(ctx.arg_i32(0));
    if (old_dirfd < 0) {
        return old_dirfd;
    }
    std::string old_path;
    std::int64_t rc = ctx.read_cstr(ctx.arg(1), old_path);
    if (rc != 0) {
        return rc;
    }
    const std::int64_t new_dirfd = ctx.host_dirfd(ctx.arg_i32(2));
    if (new_dirfd < 0) {
        return new_dirfd;
    }
    std::string new_path;
    rc = ctx.read_cstr(ctx.arg(3), new_path);
    if (rc != 0) {
        return rc;
    }
    const std::uint64_t flags = ctx.arg(4);
    if ((flags & ~static_cast<std::uint64_t>(AT_SYMLINK_FOLLOW)) != 0) {
        return -kEinval;
    }
    if (::linkat(static_cast<int>(old_dirfd), ctx.resolve_path(old_path).c_str(),
                 static_cast<int>(new_dirfd), ctx.resolve_path(new_path).c_str(),
                 static_cast<int>(flags)) != 0) {
        return errno_result();
    }
    return 0;
}

std::int64_t adapter_fsync(SyscallContext& ctx) {
    const std::int64_t host = ctx.host_fd(ctx.arg_i32(0));
    if (host < 0) {
        return host;
    }
    if (::fsync(static_cast<int>(host)) != 0) {
        return errno_result();
    }
    return 0;
}

std::int64_t adapter_fdatasync(SyscallContext& ctx) {
    const std::int64_t host = ctx.host_fd(ctx.arg_i32(0));
    if (host < 0) {
        return host;
    }
    if (::fdatasync(static_cast<int>(host)) != 0) {
        return errno_result();
    }
    return 0;
}

std::int64_t adapter_sync(SyscallContext&) {
    ::sync();
    return 0;
}

std::int64_t adapter_syncfs(SyscallContext& ctx) {
    const std::int64_t host = ctx.host_fd(ctx.arg_i32(0));
    if (host < 0) {
        return host;
    }
    if (::syncfs(static_cast<int>(host)) != 0) {
        return errno_result();
    }
    return 0;
}

// --- process and scheduling ---------------------------------------------------

std::int64_t adapter_sched_yield(SyscallContext&) {
    if (::sched_yield() != 0) {
        return errno_result();
    }
    return 0;
}

// struct sched_param is a single int on both ABIs; it is still copied through
// validated guest memory.
std::int64_t adapter_sched_getparam(SyscallContext& ctx) {
    const auto pid = static_cast<pid_t>(ctx.arg_i32(0));
    struct sched_param param{};
    if (::sched_getparam(pid, &param) != 0) {
        return errno_result();
    }
    std::uint8_t raw[kGuestSchedParamSize] = {};
    syscall_detail::put_u32(raw, 0, static_cast<std::uint32_t>(param.sched_priority));
    if (!ctx.copy_out(ctx.arg(1), raw, sizeof(raw))) {
        return -kEfault;
    }
    return 0;
}

std::int64_t adapter_sched_setscheduler(SyscallContext& ctx) {
    const auto pid = static_cast<pid_t>(ctx.arg_i32(0));
    const int policy = ctx.arg_i32(1);
    std::uint8_t raw[kGuestSchedParamSize] = {};
    if (!ctx.copy_in(ctx.arg(2), raw, sizeof(raw))) {
        return -kEfault;
    }
    struct sched_param param{};
    param.sched_priority = static_cast<int>(get_u32_le(raw));
    if (::sched_setscheduler(pid, policy, &param) != 0) {
        return errno_result();
    }
    return 0;
}

std::int64_t adapter_sched_getscheduler(SyscallContext& ctx) {
    const int policy = ::sched_getscheduler(static_cast<pid_t>(ctx.arg_i32(0)));
    if (policy < 0) {
        return errno_result();
    }
    return static_cast<std::int64_t>(policy);
}

std::int64_t adapter_sched_get_priority_max(SyscallContext& ctx) {
    const int value = ::sched_get_priority_max(ctx.arg_i32(0));
    if (value < 0) {
        return errno_result();
    }
    return static_cast<std::int64_t>(value);
}

std::int64_t adapter_sched_get_priority_min(SyscallContext& ctx) {
    const int value = ::sched_get_priority_min(ctx.arg_i32(0));
    if (value < 0) {
        return errno_result();
    }
    return static_cast<std::int64_t>(value);
}

// getrusage(who, usage). `struct rusage` is 144 bytes with identical field
// layout on x86-64 and AArch64 (two timevals followed by fourteen longs), but
// only those 144 bytes are exposed to the guest so a host with a larger struct
// cannot write past what the guest allocated.
std::int64_t adapter_getrusage(SyscallContext& ctx) {
    const int who = ctx.arg_i32(0);
    struct rusage usage{};
    if (::getrusage(who, &usage) != 0) {
        return errno_result();
    }
    static_assert(sizeof(struct rusage) == kGuestRusageSize,
                  "x86-64 and AArch64 rusage layouts must agree");
    std::uint8_t raw[kGuestRusageSize] = {};
    std::memcpy(raw, &usage, sizeof(raw));
    if (!ctx.copy_out(ctx.arg(1), raw, sizeof(raw))) {
        return -kEfault;
    }
    return 0;
}

std::int64_t adapter_setsid(SyscallContext&) {
    const pid_t sid = ::setsid();
    if (sid < 0) {
        return errno_result();
    }
    return static_cast<std::int64_t>(sid);
}

std::int64_t adapter_setpgid(SyscallContext& ctx) {
    if (::setpgid(static_cast<pid_t>(ctx.arg_i32(0)), static_cast<pid_t>(ctx.arg_i32(1))) != 0) {
        return errno_result();
    }
    return 0;
}

std::int64_t adapter_getpgid(SyscallContext& ctx) {
    const pid_t pgid = ::getpgid(static_cast<pid_t>(ctx.arg_i32(0)));
    if (pgid < 0) {
        return errno_result();
    }
    return static_cast<std::int64_t>(pgid);
}

std::int64_t adapter_getpgrp(SyscallContext&) {
    const pid_t pgid = ::getpgrp();
    if (pgid < 0) {
        return errno_result();
    }
    return static_cast<std::int64_t>(pgid);
}

// membarrier(cmd, flags). HYPERION executes the guest on a single host thread, so
// the barrier is trivially satisfied; QUERY reports "no optional commands" as the
// kernel does on an unsupported host, rather than pretending to implement them.
std::int64_t adapter_membarrier(SyscallContext& ctx) {
    constexpr std::uint32_t kCmdQuery = 0;
    constexpr std::uint32_t kCmdPrivateExpedited = 8;
    const std::uint32_t cmd = static_cast<std::uint32_t>(ctx.arg(0));
    const std::uint32_t flags = static_cast<std::uint32_t>(ctx.arg(1));
    if (flags != 0) {
        return -kEinval;
    }
    if (cmd == kCmdQuery) {
        return 0;
    }
    if (cmd == kCmdPrivateExpedited) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return 0;
    }
    return -kEinval;
}

// times(buf): returns the clock tick count and fills `struct tms` (four longs,
// identical layout on both ABIs).
std::int64_t adapter_times(SyscallContext& ctx) {
    struct tms info{};
    const clock_t ticks = ::times(&info);
    if (ticks == static_cast<clock_t>(-1)) {
        return errno_result();
    }
    std::uint8_t raw[kGuestTmsSize] = {};
    const std::uint64_t fields[4] = {
        static_cast<std::uint64_t>(info.tms_utime), static_cast<std::uint64_t>(info.tms_stime),
        static_cast<std::uint64_t>(info.tms_cutime), static_cast<std::uint64_t>(info.tms_cstime)};
    for (std::size_t i = 0; i < 4; ++i) {
        syscall_detail::put_u64(raw, i * 8, fields[i]);
    }
    if (ctx.arg(0) != 0 && !ctx.copy_out(ctx.arg(0), raw, sizeof(raw))) {
        return -kEfault;
    }
    return static_cast<std::int64_t>(ticks);
}

// --- interval timers ----------------------------------------------------------

// `struct itimerval` is four timevals (64 bytes) on both ABIs, but it is read and
// written through guest memory so a bad pointer is -EFAULT rather than a crash.
std::int64_t adapter_getitimer(SyscallContext& ctx) {
    const int which = ctx.arg_i32(0);
    struct itimerval current{};
    if (::getitimer(which, &current) != 0) {
        return errno_result();
    }
    std::uint8_t raw[sizeof(struct itimerval)] = {};
    std::memcpy(raw, &current, sizeof(raw));
    static_assert(sizeof(struct itimerval) == kGuestItimervalSize,
                  "x86-64 and AArch64 itimerval layouts must agree");
    if (!ctx.copy_out(ctx.arg(1), raw, sizeof(raw))) {
        return -kEfault;
    }
    return 0;
}

std::int64_t adapter_setitimer(SyscallContext& ctx) {
    const int which = ctx.arg_i32(0);
    struct itimerval wanted{};
    if (!ctx.copy_in(ctx.arg(1), &wanted, sizeof(wanted))) {
        return -kEfault;
    }
    struct itimerval previous{};
    if (::setitimer(which, &wanted, &previous) != 0) {
        return errno_result();
    }
    if (ctx.arg(2) != 0) {
        std::uint8_t raw[sizeof(struct itimerval)] = {};
        std::memcpy(raw, &previous, sizeof(raw));
        if (!ctx.copy_out(ctx.arg(2), raw, sizeof(raw))) {
            return -kEfault;
        }
    }
    return 0;
}

// --- descriptor-based event sources -------------------------------------------

std::int64_t adapter_eventfd2(SyscallContext& ctx) {
    constexpr std::uint64_t kEfdSemaphore = 1;
    constexpr std::uint64_t kEfdNonblock = 0x800;
    constexpr std::uint64_t kEfdCloseexec = 0x80000;
    constexpr std::uint64_t kKnown = kEfdSemaphore | kEfdNonblock | kEfdCloseexec;
    const std::uint64_t flags = ctx.arg(1);
    if ((flags & ~kKnown) != 0) {
        return -kEinval;
    }
    int host_flags = EFD_CLOEXEC; // never leak a descriptor into a host child
    if ((flags & kEfdNonblock) != 0) {
        host_flags |= EFD_NONBLOCK;
    }
    if ((flags & kEfdSemaphore) != 0) {
        host_flags |= EFD_SEMAPHORE;
    }
    const int fd = ::eventfd(static_cast<unsigned int>(ctx.arg(0) & 0xFFFFFFFFULL), host_flags);
    if (fd < 0) {
        return errno_result();
    }
    const bool cloexec = (flags & kEfdCloseexec) != 0;
    const std::int64_t guest_fd = ctx.add_host_fd(fd, cloexec);
    if (guest_fd < 0) {
        ::close(fd);
        return guest_fd;
    }
    return guest_fd;
}

std::int64_t adapter_epoll_create1(SyscallContext& ctx) {
    constexpr std::uint64_t kEpollCloseexec = 0x80000;
    const std::uint64_t flags = ctx.arg(0);
    if ((flags & ~kEpollCloseexec) != 0) {
        return -kEinval;
    }
    const int fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) {
        return errno_result();
    }
    const std::int64_t guest_fd = ctx.add_host_fd(fd, (flags & kEpollCloseexec) != 0);
    if (guest_fd < 0) {
        ::close(fd);
        return guest_fd;
    }
    return guest_fd;
}

// memfd_create(2), or the closest thing the host allows.
//
// On Android before API 30 memfd_create is outside the app seccomp allowlist and
// issuing it is a fatal SIGSYS (SECCOMP_RET_TRAP), not an errno - see
// hyperion/host_syscalls.hpp. There is no way to probe for it, so the decision is
// taken from the device's API level.
//
// The substitute is O_TMPFILE on a writable temporary directory: an unnamed,
// already-unlinked file with the same read/write/mmap behaviour, which is what a
// guest uses an memfd for in practice (shared anonymous memory, seals excepted).
// It is not identical to memfd_create:
//   - F_ADD_SEALS/F_GET_SEALS are not supported on the resulting descriptor, so
//     MFD_ALLOW_SEALING is accepted but not honoured;
//   - the file lives on the temporary filesystem, so it consumes that space.
// When O_TMPFILE is unavailable the call reports -ENOSYS, which is a documented
// answer a guest can fall back from. A wrong "success" is never fabricated.
std::int64_t create_anonymous_file(const std::string& name, unsigned int host_flags, bool cloexec) {
    const int open_flags = O_RDWR | O_TMPFILE | (cloexec ? O_CLOEXEC : 0);
    (void)name;
    if (host_memfd_create_syscall_usable()) {
#ifdef SYS_memfd_create
        // Issued through syscall(2) rather than ::memfd_create so the binary does
        // not require a libc symbol that only exists from Android 11 onwards.
        const long fd = ::syscall(SYS_memfd_create, name.c_str(), host_flags);
        if (fd >= 0) {
            return static_cast<std::int64_t>(fd);
        }
        if (errno != ENOSYS) {
            return errno_result();
        }
#else
        (void)host_flags;
#endif
    }
    std::vector<std::string> bases;
    if (const char* tmpdir = std::getenv("TMPDIR"); tmpdir != nullptr && *tmpdir != '\0') {
        bases.emplace_back(tmpdir);
    }
    if (const char* prefix = std::getenv("PREFIX"); prefix != nullptr && *prefix != '\0') {
        bases.emplace_back(std::string(prefix) + "/tmp");
    }
    bases.emplace_back("/tmp");
    for (const std::string& base : bases) {
        const int fd = ::open(base.c_str(), open_flags, 0600);
        if (fd >= 0) {
            return static_cast<std::int64_t>(fd);
        }
    }
    return -kEnosys;
}

std::int64_t adapter_memfd_create(SyscallContext& ctx) {
    constexpr std::uint64_t kMfdCloexec = 0x1;
    constexpr std::uint64_t kMfdAllowSealing = 0x2;
    constexpr std::uint64_t kKnown = kMfdCloexec | kMfdAllowSealing;
    std::string name;
    const std::int64_t rc = ctx.read_cstr(ctx.arg(0), name);
    if (rc != 0) {
        return rc;
    }
    const std::uint64_t flags = ctx.arg(1) & 0xFFFFFFFFULL;
    if ((flags & ~kKnown) != 0) {
        return -kEinval;
    }
    unsigned int host_flags = 0;
    if ((flags & kMfdCloexec) != 0) {
        host_flags |= static_cast<unsigned int>(MFD_CLOEXEC);
    }
    if ((flags & kMfdAllowSealing) != 0) {
        host_flags |= static_cast<unsigned int>(MFD_ALLOW_SEALING);
    }
    const std::int64_t created =
        create_anonymous_file(name, host_flags, (flags & kMfdCloexec) != 0);
    if (created < 0) {
        return created;
    }
    const int fd = static_cast<int>(created);
    const std::int64_t guest_fd = ctx.add_host_fd(fd, (flags & kMfdCloexec) != 0);
    if (guest_fd < 0) {
        ::close(fd);
        return guest_fd;
    }
    return guest_fd;
}

std::int64_t adapter_timerfd_create(SyscallContext& ctx) {
    constexpr std::uint64_t kTfdNonblock = 0x800;
    constexpr std::uint64_t kTfdCloseexec = 0x80000;
    constexpr std::uint64_t kKnown = kTfdNonblock | kTfdCloseexec;
    const int clockid = ctx.arg_i32(0);
    const std::uint64_t flags = ctx.arg(1);
    if ((flags & ~kKnown) != 0) {
        return -kEinval;
    }
    int host_flags = TFD_CLOEXEC;
    if ((flags & kTfdNonblock) != 0) {
        host_flags |= TFD_NONBLOCK;
    }
    const int fd = ::timerfd_create(clockid, host_flags);
    if (fd < 0) {
        return errno_result();
    }
    const std::int64_t guest_fd = ctx.add_host_fd(fd, (flags & kTfdCloseexec) != 0);
    if (guest_fd < 0) {
        ::close(fd);
        return guest_fd;
    }
    return guest_fd;
}

std::int64_t adapter_timerfd_settime(SyscallContext& ctx) {
    constexpr std::uint64_t kTfdTimerAbstime = 1;
    constexpr std::uint64_t kTfdTimerCancelOnSet = 2;
    constexpr std::uint64_t kKnown = kTfdTimerAbstime | kTfdTimerCancelOnSet;
    const std::int64_t host = ctx.host_fd(ctx.arg_i32(0));
    if (host < 0) {
        return host;
    }
    const std::uint64_t flags = ctx.arg(1);
    if ((flags & ~kKnown) != 0) {
        return -kEinval;
    }
    struct itimerspec wanted{};
    if (!ctx.copy_in(ctx.arg(2), &wanted, sizeof(wanted))) {
        return -kEfault;
    }
    struct itimerspec previous{};
    if (::timerfd_settime(static_cast<int>(host), static_cast<int>(flags), &wanted, &previous) !=
        0) {
        return errno_result();
    }
    if (ctx.arg(3) != 0 && !ctx.copy_out(ctx.arg(3), &previous, sizeof(previous))) {
        return -kEfault;
    }
    return 0;
}

std::int64_t adapter_timerfd_gettime(SyscallContext& ctx) {
    const std::int64_t host = ctx.host_fd(ctx.arg_i32(0));
    if (host < 0) {
        return host;
    }
    struct itimerspec current{};
    if (::timerfd_gettime(static_cast<int>(host), &current) != 0) {
        return errno_result();
    }
    if (!ctx.copy_out(ctx.arg(1), &current, sizeof(current))) {
        return -kEfault;
    }
    return 0;
}

// --- epoll --------------------------------------------------------------------

// Translate one guest `struct epoll_event` (12 packed bytes) into the host layout.
bool read_guest_epoll_event(SyscallContext& ctx, std::uint64_t address, struct epoll_event& out) {
    std::uint8_t raw[kGuestEpollEventSize] = {};
    if (!ctx.copy_in(address, raw, sizeof(raw))) {
        return false;
    }
    out.events = syscall_detail::get_u32_le(raw);
    out.data.u64 = syscall_detail::get_u64_le(raw + kGuestEpollEventDataOffset);
    return true;
}

bool write_guest_epoll_event(SyscallContext& ctx, std::uint64_t address,
                             const struct epoll_event& event) {
    std::uint8_t raw[kGuestEpollEventSize] = {};
    syscall_detail::put_u32(raw, 0, static_cast<std::uint32_t>(event.events));
    syscall_detail::put_u64(raw, kGuestEpollEventDataOffset, event.data.u64);
    return ctx.copy_out(address, raw, sizeof(raw));
}

std::int64_t adapter_epoll_ctl(SyscallContext& ctx) {
    constexpr int kEpollCtlAdd = 1;
    constexpr int kEpollCtlDel = 2;
    constexpr int kEpollCtlMod = 3;
    const std::int64_t epfd = ctx.host_fd(ctx.arg_i32(0));
    if (epfd < 0) {
        return epfd;
    }
    const int op = ctx.arg_i32(1);
    if (op != kEpollCtlAdd && op != kEpollCtlDel && op != kEpollCtlMod) {
        return -kEinval;
    }
    const std::int64_t fd = ctx.host_fd(ctx.arg_i32(2));
    if (fd < 0) {
        return fd;
    }
    struct epoll_event event{};
    if (op != kEpollCtlDel) {
        // A NULL event is only legal for EPOLL_CTL_DEL; anything else must read
        // the guest's descriptor, and an unreadable pointer is -EFAULT.
        if (ctx.arg(3) == 0) {
            return -kEfault;
        }
        if (!read_guest_epoll_event(ctx, ctx.arg(3), event)) {
            return -kEfault;
        }
    }
    if (::epoll_ctl(static_cast<int>(epfd), op, static_cast<int>(fd), &event) != 0) {
        return errno_result();
    }
    return 0;
}

// Shared body of epoll_pwait (millisecond timeout) and epoll_pwait2 (guest
// timespec timeout). The guest's epoll_event array is 12 packed bytes per entry;
// the host's is 16, so entries are converted one at a time and never memcpy'd.
std::int64_t epoll_wait_common(SyscallContext& ctx, bool timespec_timeout, bool has_sigmask_args) {
    const std::int64_t epfd = ctx.host_fd(ctx.arg_i32(0));
    if (epfd < 0) {
        return epfd;
    }
    const std::uint64_t events_address = ctx.arg(1);
    const std::int32_t maxevents = ctx.arg_i32(2);
    if (maxevents <= 0) {
        return -kEinval;
    }
    // The kernel caps epoll_wait at EP_MAX_EVENTS; mirroring the cap keeps a
    // hostile `maxevents` from driving a large host allocation.
    constexpr std::int32_t kMaxEvents = 4096;
    if (maxevents > kMaxEvents) {
        return -kEinval;
    }
    const std::uint64_t timeout_address = ctx.arg(3);
    // epoll_wait(2) has no signal-mask arguments at all: reading r8/r9 for it
    // would interpret caller scratch as a pointer.
    const std::uint64_t sigmask_address = has_sigmask_args ? ctx.arg(4) : 0;
    const std::uint64_t sigsetsize = has_sigmask_args ? ctx.arg(5) : 0;

    if (sigmask_address != 0 && sigsetsize != 8) {
        // The guest's signal mask is modelled inside the handler, not by the host
        // process, so the substitution itself cannot be forwarded. The size is
        // still validated exactly as the kernel does, so a guest that passes a
        // wrong sigsetsize sees the same EINVAL it would natively.
        return -kEinval;
    }

    int timeout_ms = -1;
    if (timespec_timeout) {
        // epoll_pwait2(2) is the only variant whose timeout is a pointer.
        if (timeout_address != 0) {
            struct timespec spec{};
            if (!ctx.copy_in(timeout_address, &spec, kGuestTimespecSize)) {
                return -kEfault;
            }
            if (spec.tv_sec < 0 || spec.tv_nsec < 0 || spec.tv_nsec >= 1000000000L) {
                return -kEinval;
            }
            // Round up so a sub-millisecond timeout never becomes a spin.
            const std::int64_t millis =
                static_cast<std::int64_t>(spec.tv_sec) * 1000 +
                (static_cast<std::int64_t>(spec.tv_nsec) + 999999) / 1000000;
            timeout_ms = millis > INT_MAX ? INT_MAX : static_cast<int>(millis);
        }
    } else {
        // epoll_wait/epoll_pwait take the millisecond timeout *by value* in the
        // fourth argument register, exactly as the kernel does. Treating it as a
        // pointer (the natural mistake) turns every call into -EFAULT.
        const std::int32_t millis = static_cast<std::int32_t>(timeout_address);
        timeout_ms = millis < 0 ? -1 : static_cast<int>(millis);
    }

    std::vector<struct epoll_event> host_events(static_cast<std::size_t>(maxevents));
    const int ready =
        ::epoll_wait(static_cast<int>(epfd), host_events.data(), maxevents, timeout_ms);
    if (ready < 0) {
        return errno_result();
    }
    for (int i = 0; i < ready; ++i) {
        const std::uint64_t address =
            events_address + static_cast<std::uint64_t>(i) * kGuestEpollEventSize;
        if (!write_guest_epoll_event(ctx, address, host_events[static_cast<std::size_t>(i)])) {
            return -kEfault;
        }
    }
    return static_cast<std::int64_t>(ready);
}

std::int64_t adapter_epoll_wait(SyscallContext& ctx) {
    return epoll_wait_common(ctx, false, false);
}

std::int64_t adapter_epoll_pwait(SyscallContext& ctx) {
    return epoll_wait_common(ctx, false, true);
}

std::int64_t adapter_epoll_pwait2(SyscallContext& ctx) {
    return epoll_wait_common(ctx, true, true);
}

// --- poll / select ------------------------------------------------------------

// Shared body of poll(2) and ppoll(2). `timeout_ms` is already resolved; the
// guest's `struct pollfd` array is translated entry by entry because the guest's
// descriptor numbers are not the host's.
std::int64_t poll_common(SyscallContext& ctx, int timeout_ms) {
    const std::uint64_t fds_address = ctx.arg(0);
    const std::int32_t nfds = ctx.arg_i32(1);
    if (nfds < 0) {
        return -kEinval;
    }
    constexpr std::int32_t kMaxFds = 4096;
    if (nfds > kMaxFds) {
        return -kEinval;
    }
    std::vector<struct pollfd> host_fds(static_cast<std::size_t>(nfds));
    std::vector<std::int32_t> guest_fds_backup(static_cast<std::size_t>(nfds));
    for (std::int32_t i = 0; i < nfds; ++i) {
        std::uint8_t raw[kGuestPollfdSize] = {};
        if (!ctx.copy_in(fds_address + static_cast<std::uint64_t>(i) * kGuestPollfdSize, raw,
                         sizeof(raw))) {
            return -kEfault;
        }
        // struct pollfd is exactly 8 bytes: int fd; short events; short revents.
        // Read each field with a reader of its own width - a 64-bit read of the
        // 16-bit `events` field ran four bytes past the end of `raw`.
        const std::int32_t guest_fd = static_cast<std::int32_t>(syscall_detail::get_u32_le(raw));
        const auto events = static_cast<std::int16_t>(syscall_detail::get_u16_le(raw + 4));
        guest_fds_backup[static_cast<std::size_t>(i)] = guest_fd;
        if (guest_fd < 0) {
            host_fds[static_cast<std::size_t>(i)].fd = -1;
        } else {
            const std::int64_t host = ctx.host_fd(guest_fd);
            if (host < 0) {
                return host;
            }
            host_fds[static_cast<std::size_t>(i)].fd = static_cast<int>(host);
        }
        host_fds[static_cast<std::size_t>(i)].events = events;
        host_fds[static_cast<std::size_t>(i)].revents = 0;
    }
    const int ready = ::poll(host_fds.data(), static_cast<nfds_t>(nfds), timeout_ms);
    if (ready < 0) {
        return errno_result();
    }
    for (std::int32_t i = 0; i < nfds; ++i) {
        const std::size_t index = static_cast<std::size_t>(i);
        std::uint8_t raw[kGuestPollfdSize] = {};
        syscall_detail::put_u32(raw, 0, static_cast<std::uint32_t>(guest_fds_backup[index]));
        // struct pollfd { int fd; short events; short revents; }
        // `events` is an input the kernel leaves alone, so it is written back
        // unchanged rather than dropped.
        syscall_detail::put_u16(raw, 4, static_cast<std::uint16_t>(host_fds[index].events));
        syscall_detail::put_u16(raw, 6, static_cast<std::uint16_t>(host_fds[index].revents));
        if (!ctx.copy_out(fds_address + static_cast<std::uint64_t>(i) * kGuestPollfdSize, raw,
                          sizeof(raw))) {
            return -kEfault;
        }
    }
    return static_cast<std::int64_t>(ready);
}

std::int64_t adapter_poll(SyscallContext& ctx) {
    return poll_common(ctx, ctx.arg_i32(2));
}

std::int64_t adapter_ppoll(SyscallContext& ctx) {
    const std::uint64_t timeout_address = ctx.arg(2);
    const std::uint64_t sigmask_address = ctx.arg(3);
    const std::uint64_t sigsetsize = ctx.arg(4);
    if (sigmask_address != 0 && sigsetsize != 8) {
        return -kEinval;
    }
    int timeout_ms = -1;
    if (timeout_address != 0) {
        struct timespec spec{};
        if (!ctx.copy_in(timeout_address, &spec, kGuestTimespecSize)) {
            return -kEfault;
        }
        if (spec.tv_sec < 0 || spec.tv_nsec < 0 || spec.tv_nsec >= 1000000000L) {
            return -kEinval;
        }
        const std::int64_t millis = static_cast<std::int64_t>(spec.tv_sec) * 1000 +
                                    (static_cast<std::int64_t>(spec.tv_nsec) + 999999) / 1000000;
        timeout_ms = millis > INT_MAX ? INT_MAX : static_cast<int>(millis);
    }
    return poll_common(ctx, timeout_ms);
}

// Shared poll/select file-descriptor-set translation.
//
// The guest and host descriptor spaces do not coincide, so a bitmask cannot be
// forwarded: each guest fd named by the guest is resolved to its host fd, the
// host call runs on host descriptors, and the result is mapped back by scanning
// the same guest range again.
struct SelectSets {
    fd_set read_set{};
    fd_set write_set{};
    fd_set except_set{};
    bool has_read = false;
    bool has_write = false;
    bool has_except = false;
};

bool read_guest_fd_set(SyscallContext& ctx, std::uint64_t address, std::uint8_t* raw) {
    return ctx.copy_in(address, raw, kGuestFdSetSize);
}

std::int64_t select_common(SyscallContext& ctx, bool pselect) {
    const std::int32_t nfds = ctx.arg_i32(0);
    if (nfds < 0) {
        return -kEinval;
    }
    if (nfds > kGuestFdSetMax) {
        return -kEinval;
    }
    const std::uint64_t read_address = ctx.arg(1);
    const std::uint64_t write_address = ctx.arg(2);
    const std::uint64_t except_address = ctx.arg(3);
    const std::uint64_t timeout_address = ctx.arg(4);

    std::uint8_t guest_read[kGuestFdSetSize] = {};
    std::uint8_t guest_write[kGuestFdSetSize] = {};
    std::uint8_t guest_except[kGuestFdSetSize] = {};
    if (read_address != 0 && !read_guest_fd_set(ctx, read_address, guest_read)) {
        return -kEfault;
    }
    if (write_address != 0 && !read_guest_fd_set(ctx, write_address, guest_write)) {
        return -kEfault;
    }
    if (except_address != 0 && !read_guest_fd_set(ctx, except_address, guest_except)) {
        return -kEfault;
    }

    SelectSets host_sets;
    int max_host_fd = -1;
    for (std::int32_t fd = 0; fd < nfds; ++fd) {
        const auto index = static_cast<std::size_t>(fd);
        const bool in_read = (guest_read[index / 8] & (1U << (fd % 8))) != 0;
        const bool in_write = (guest_write[index / 8] & (1U << (fd % 8))) != 0;
        const bool in_except = (guest_except[index / 8] & (1U << (fd % 8))) != 0;
        if (!in_read && !in_write && !in_except) {
            continue;
        }
        const std::int64_t host = ctx.host_fd(fd);
        if (host < 0) {
            return host;
        }
        const auto host_index = static_cast<std::size_t>(host);
        if (host >= kGuestFdSetMax) {
            return -kEbadf; // host descriptor outside the guest's fd_set width
        }
        if (in_read) {
            FD_SET(static_cast<int>(host_index), &host_sets.read_set);
            host_sets.has_read = true;
        }
        if (in_write) {
            FD_SET(static_cast<int>(host_index), &host_sets.write_set);
            host_sets.has_write = true;
        }
        if (in_except) {
            FD_SET(static_cast<int>(host_index), &host_sets.except_set);
            host_sets.has_except = true;
        }
        if (static_cast<int>(host) > max_host_fd) {
            max_host_fd = static_cast<int>(host);
        }
    }

    struct timeval timeout{};
    struct timeval* timeout_ptr = nullptr;
    int host_timeout_ms = -1;
    if (timeout_address != 0) {
        if (pselect) {
            struct timespec spec{};
            if (!ctx.copy_in(timeout_address, &spec, kGuestTimespecSize)) {
                return -kEfault;
            }
            timeout.tv_sec = spec.tv_sec;
            timeout.tv_usec = spec.tv_nsec / 1000;
            timeout_ptr = &timeout;
            host_timeout_ms = static_cast<int>(timeout.tv_sec * 1000 + timeout.tv_usec / 1000);
        } else {
            if (!ctx.copy_in(timeout_address, &timeout, kGuestTimevalSize)) {
                return -kEfault;
            }
            timeout_ptr = &timeout;
            host_timeout_ms = static_cast<int>(timeout.tv_sec * 1000 + timeout.tv_usec / 1000);
        }
    }

    // pselect6 passes a pointer to {const sigset_t* ss; size_t ss_len}.
    if (pselect && ctx.arg(5) != 0) {
        std::uint8_t pair[16] = {};
        if (!ctx.copy_in(ctx.arg(5), pair, sizeof(pair))) {
            return -kEfault;
        }
        const std::uint64_t mask_length = syscall_detail::get_u64_le(pair + 8);
        if (mask_length != 8) {
            return -kEinval;
        }
    }

    int ready = 0;
    if (host_timeout_ms >= 0) {
        ready = ::select(max_host_fd + 1, &host_sets.read_set, &host_sets.write_set,
                         &host_sets.except_set, timeout_ptr);
    } else {
        ready = ::select(max_host_fd + 1, &host_sets.read_set, &host_sets.write_set,
                         &host_sets.except_set, nullptr);
    }
    if (ready < 0) {
        return errno_result();
    }

    std::uint8_t out_read[kGuestFdSetSize] = {};
    std::uint8_t out_write[kGuestFdSetSize] = {};
    std::uint8_t out_except[kGuestFdSetSize] = {};
    for (std::int32_t fd = 0; fd < nfds; ++fd) {
        const std::int64_t host = ctx.host_fd(fd);
        if (host < 0) {
            continue;
        }
        const auto host_index = static_cast<std::size_t>(host);
        if (FD_ISSET(static_cast<int>(host_index), &host_sets.read_set)) {
            out_read[static_cast<std::size_t>(fd) / 8] |=
                static_cast<std::uint8_t>(1U << (static_cast<unsigned>(fd) % 8));
        }
        if (FD_ISSET(static_cast<int>(host_index), &host_sets.write_set)) {
            out_write[static_cast<std::size_t>(fd) / 8] |=
                static_cast<std::uint8_t>(1U << (static_cast<unsigned>(fd) % 8));
        }
        if (FD_ISSET(static_cast<int>(host_index), &host_sets.except_set)) {
            out_except[static_cast<std::size_t>(fd) / 8] |=
                static_cast<std::uint8_t>(1U << (static_cast<unsigned>(fd) % 8));
        }
    }
    if (read_address != 0 && !ctx.copy_out(read_address, out_read, kGuestFdSetSize)) {
        return -kEfault;
    }
    if (write_address != 0 && !ctx.copy_out(write_address, out_write, kGuestFdSetSize)) {
        return -kEfault;
    }
    if (except_address != 0 && !ctx.copy_out(except_address, out_except, kGuestFdSetSize)) {
        return -kEfault;
    }
    return static_cast<std::int64_t>(ready);
}

std::int64_t adapter_select(SyscallContext& ctx) {
    return select_common(ctx, false);
}
std::int64_t adapter_pselect6(SyscallContext& ctx) {
    return select_common(ctx, true);
}

// --- the compiled-in whitelist ------------------------------------------------

const AdapterDescriptor kAdapterTable[] = {
    {"compat.poll", 7, "poll", adapter_poll, kAdapterNone,
     "poll(2) over an arbitrary set of guest descriptors", kPollWait},
    {"compat.select", 23, "select", adapter_select, kAdapterNone,
     "select(2) with guest/host descriptor translation", kSelectWait},
    {"compat.sched_yield", 24, "sched_yield", adapter_sched_yield, kAdapterNone,
     "yield the processor", nullptr},
    {"compat.getitimer", 36, "getitimer", adapter_getitimer, kAdapterNone, "read an interval timer",
     kItimer},
    {"compat.setitimer", 38, "setitimer", adapter_setitimer, kAdapterNone, "arm an interval timer",
     kItimer},
    {"compat.fsync", 74, "fsync", adapter_fsync, kAdapterNone,
     "flush a descriptor to stable storage", kFdOnly},
    {"compat.fdatasync", 75, "fdatasync", adapter_fdatasync, kAdapterNone,
     "flush data (not metadata) to stable storage", kFdOnly},
    {"compat.truncate", 76, "truncate", adapter_truncate, kAdapterNone,
     "truncate a path to a length", kPathOnly},
    {"compat.ftruncate", 77, "ftruncate", adapter_ftruncate, kAdapterNone,
     "truncate a descriptor to a length", kFdOnly},
    {"compat.chdir", 80, "chdir", adapter_chdir, kAdapterNone, "change the working directory",
     kPathOnly},
    {"compat.fchdir", 81, "fchdir", adapter_fchdir, kAdapterNone,
     "change the working directory to a descriptor's directory", kFdOnly},
    {"compat.umask", 95, "umask", adapter_umask, kAdapterNone,
     "set and return the file mode creation mask", nullptr},
    {"compat.getrusage", 98, "getrusage", adapter_getrusage, kAdapterNone, "report resource usage",
     kRusageList},
    {"compat.times", 100, "times", adapter_times, kAdapterNone, "report process and child times",
     kTimesList},
    {"compat.setpgid", 109, "setpgid", adapter_setpgid, kAdapterNone, "set a process group",
     nullptr},
    {"compat.getpgrp", 111, "getpgrp", adapter_getpgrp, kAdapterNone, "return the process group",
     nullptr},
    {"compat.setsid", 112, "setsid", adapter_setsid, kAdapterNone, "create a new session", nullptr},
    {"compat.getpgid", 121, "getpgid", adapter_getpgid, kAdapterNone, "return a process's group",
     nullptr},
    {"compat.sched_getparam", 143, "sched_getparam", adapter_sched_getparam, kAdapterNone,
     "read a scheduling parameter block", kSchedParamList},
    {"compat.sched_setscheduler", 144, "sched_setscheduler", adapter_sched_setscheduler,
     kAdapterNone, "set the scheduling policy and parameters", kSchedParamList},
    {"compat.sched_getscheduler", 145, "sched_getscheduler", adapter_sched_getscheduler,
     kAdapterNone, "read the scheduling policy", nullptr},
    {"compat.sched_get_priority_max", 146, "sched_get_priority_max", adapter_sched_get_priority_max,
     kAdapterNone, "read the maximum priority for a policy", nullptr},
    {"compat.sched_get_priority_min", 147, "sched_get_priority_min", adapter_sched_get_priority_min,
     kAdapterNone, "read the minimum priority for a policy", nullptr},
    {"compat.sync", 162, "sync", adapter_sync, kAdapterNone, "flush all filesystems", nullptr},
    {"compat.epoll_wait", 232, "epoll_wait", adapter_epoll_wait, kAdapterNone,
     "wait for epoll events (millisecond timeout)", kEpollWait},
    {"compat.epoll_ctl", 233, "epoll_ctl", adapter_epoll_ctl, kAdapterNone,
     "add/modify/remove a guest descriptor from an epoll set", kEpollCtl},
    {"compat.linkat", 265, "linkat", adapter_linkat, kAdapterNone,
     "create a hard link relative to directory descriptors", kPathAndBuffer},
    {"compat.symlinkat", 266, "symlinkat", adapter_symlinkat, kAdapterNone,
     "create a symbolic link relative to a directory descriptor", kPathAndBuffer},
    {"compat.pselect6", 270, "pselect6", adapter_pselect6, kAdapterNone,
     "pselect(2) with guest/host descriptor translation", kSelectWait},
    {"compat.ppoll", 271, "ppoll", adapter_ppoll, kAdapterNone, "ppoll(2) with a timespec timeout",
     kPollWait},
    {"compat.epoll_pwait", 281, "epoll_pwait", adapter_epoll_pwait, kAdapterNone,
     "wait for epoll events (millisecond timeout)", kEpollWait},
    {"compat.timerfd_create", 283, "timerfd_create", adapter_timerfd_create, kAdapterNone,
     "create a timer that reports expirations through a descriptor", kFdOnly},
    {"compat.timerfd_settime", 286, "timerfd_settime", adapter_timerfd_settime, kAdapterNone,
     "arm or disarm a timerfd", kTimerfdSet},
    {"compat.timerfd_gettime", 287, "timerfd_gettime", adapter_timerfd_gettime, kAdapterNone,
     "read the remaining time of a timerfd", kFdAndBuffer},
    {"compat.eventfd2", 290, "eventfd2", adapter_eventfd2, kAdapterNone,
     "create an event notification descriptor", kFdOnly},
    {"compat.epoll_create1", 291, "epoll_create1", adapter_epoll_create1, kAdapterNone,
     "create an epoll instance", kFdOnly},
    {"compat.syncfs", 306, "syncfs", adapter_syncfs, kAdapterNone,
     "flush the filesystem containing a descriptor", kFdOnly},
    {"compat.memfd_create", 319, "memfd_create", adapter_memfd_create, kAdapterNone,
     "create an anonymous memory-backed file", kFdOnly},
    {"compat.membarrier", 324, "membarrier", adapter_membarrier, kAdapterNone,
     "issue a memory barrier", nullptr},
    {"compat.epoll_pwait2", 441, "epoll_pwait2", adapter_epoll_pwait2, kAdapterNone,
     "wait for epoll events (nanosecond timeout)", kEpollWaitMasked},
};

constexpr std::size_t kAdapterCount = sizeof(kAdapterTable) / sizeof(kAdapterTable[0]);

// A small symbolic-name table for the numbers the existing handler implements, so
// an unsupported-syscall report can name them instead of printing a bare number.
struct NameEntry {
    std::uint64_t number;
    const char* name;
};

const NameEntry kExistingNames[] = {
    {0, "read"},
    {1, "write"},
    {2, "open"},
    {3, "close"},
    {4, "stat"},
    {5, "fstat"},
    {6, "lstat"},
    {8, "lseek"},
    {9, "mmap"},
    {10, "mprotect"},
    {11, "munmap"},
    {12, "brk"},
    {13, "rt_sigaction"},
    {14, "rt_sigprocmask"},
    {15, "rt_sigreturn"},
    {16, "ioctl"},
    {17, "pread64"},
    {18, "pwrite64"},
    {19, "readv"},
    {20, "writev"},
    {21, "access"},
    {22, "pipe"},
    {25, "mremap"},
    {28, "madvise"},
    {32, "dup"},
    {33, "dup2"},
    {35, "nanosleep"},
    {39, "getpid"},
    {56, "clone"},
    {57, "fork"},
    {58, "vfork"},
    {59, "execve"},
    {60, "exit"},
    {61, "wait4"},
    {62, "kill"},
    {63, "uname"},
    {72, "fcntl"},
    {79, "getcwd"},
    {82, "rename"},
    {83, "mkdir"},
    {84, "rmdir"},
    {87, "unlink"},
    {89, "readlink"},
    {90, "chmod"},
    {91, "fchmod"},
    {96, "gettimeofday"},
    {97, "getrlimit"},
    {99, "sysinfo"},
    {102, "getuid"},
    {104, "getgid"},
    {107, "geteuid"},
    {108, "getegid"},
    {110, "getppid"},
    {131, "sigaltstack"},
    {137, "statfs"},
    {138, "fstatfs"},
    {157, "prctl"},
    {158, "arch_prctl"},
    {160, "setrlimit"},
    {186, "gettid"},
    {202, "futex"},
    {204, "sched_getaffinity"},
    {217, "getdents64"},
    {218, "set_tid_address"},
    {221, "fadvise64"},
    {228, "clock_gettime"},
    {229, "clock_getres"},
    {230, "clock_nanosleep"},
    {231, "exit_group"},
    {234, "tgkill"},
    {257, "openat"},
    {258, "mkdirat"},
    {262, "newfstatat"},
    {263, "unlinkat"},
    {264, "renameat"},
    {267, "readlinkat"},
    {268, "fchmodat"},
    {269, "faccessat"},
    {273, "set_robust_list"},
    {280, "utimensat"},
    {292, "dup3"},
    {293, "pipe2"},
    {302, "prlimit64"},
    {316, "renameat2"},
    {318, "getrandom"},
    {332, "statx"},
    {334, "rseq"},
    {435, "clone3"},
    {439, "faccessat2"},
};

} // namespace

const AdapterDescriptor* builtin_adapters(std::size_t& count) {
    count = kAdapterCount;
    return kAdapterTable;
}

const AdapterDescriptor* find_builtin_adapter(std::string_view id) {
    for (const AdapterDescriptor& adapter : kAdapterTable) {
        if (id == adapter.id) {
            return &adapter;
        }
    }
    return nullptr;
}

const AdapterDescriptor* find_builtin_adapter_by_syscall(std::uint64_t number) {
    for (const AdapterDescriptor& adapter : kAdapterTable) {
        if (adapter.syscall == number) {
            return &adapter;
        }
    }
    return nullptr;
}

bool is_compat_adapter_syscall(std::uint64_t number) {
    return find_builtin_adapter_by_syscall(number) != nullptr;
}

const char* guest_syscall_name(std::uint64_t number) {
    if (const AdapterDescriptor* adapter = find_builtin_adapter_by_syscall(number)) {
        return adapter->name;
    }
    for (const NameEntry& entry : kExistingNames) {
        if (entry.number == number) {
            return entry.name;
        }
    }
    return nullptr;
}

RegistryDocument default_registry_document() {
    // The shipped defaults are generated from the adapter table rather than
    // written out by hand, so a rule can never name an adapter that does not
    // exist and an adapter can never be unreachable from the registry.
    RegistryDocument document;
    document.schema_version = kRegistrySchemaVersion;
    document.guest_arch = "x86_64";
    document.host_arch.clear(); // host-agnostic
    document.generated_by = "hyperion built-in defaults";
    document.comment =
        "Versioned default rules shipped with HYPERION. Merged into the user's registry by rule "
        "id; user edits to a rule with the same id take precedence.";

    for (std::size_t i = 0; i < kAdapterCount; ++i) {
        const AdapterDescriptor& adapter = kAdapterTable[i];
        RegistryRule rule;
        rule.id = std::string("hyperion.default.") + adapter.name;
        rule.name = adapter.name;
        rule.syscall = adapter.syscall;
        rule.implementation = RuleImplementation::kBuiltinAdapter;
        rule.handler = adapter.id;
        rule.return_transform = kRegistryReturnTransformErrno;
        for (const char* const* t = adapter.transforms; t != nullptr && *t != nullptr; ++t) {
            rule.transforms.emplace_back(*t);
        }
        if ((adapter.flags & kAdapterNeedsLinuxCompat) != 0) {
            rule.requirements.emplace_back("linux_compat");
        }
        rule.rule_version = 1;
        rule.origin = RuleOrigin::kPackage;
        rule.origin_detail = "generated from the compiled-in adapter table";
        rule.enabled = true;
        rule.notes = adapter.summary == nullptr ? "" : adapter.summary;
        document.rules.push_back(std::move(rule));
    }

    // Alias rules: guest syscall numbers that the existing layer already
    // implements under a different number and argument order. These demonstrate
    // the "translate semantics, not just numbers" path without adding code.
    {
        RegistryRule rule;
        rule.id = "hyperion.default.getrlimit";
        rule.name = "getrlimit";
        rule.syscall = 97;
        rule.implementation = RuleImplementation::kGuestAlias;
        rule.target_syscall = 302; // prlimit64(0, resource, NULL, old_rlim)
        rule.arg_source = {{AliasArg::Kind::kConst, 0},
                           {AliasArg::Kind::kArg, 0},
                           {AliasArg::Kind::kConst, 0},
                           {AliasArg::Kind::kArg, 1}};
        rule.return_transform = kRegistryReturnTransformErrno;
        rule.rule_version = 1;
        rule.origin = RuleOrigin::kPackage;
        rule.origin_detail = "generated from the compiled-in adapter table";
        rule.enabled = true;
        rule.notes =
            "getrlimit(resource, rlim) is prlimit64(0, resource, NULL, rlim); struct rlimit is "
            "two 64-bit fields on both ABIs";
        document.rules.push_back(std::move(rule));
    }
    {
        RegistryRule rule;
        rule.id = "hyperion.default.setrlimit";
        rule.name = "setrlimit";
        rule.syscall = 160;
        rule.implementation = RuleImplementation::kGuestAlias;
        rule.target_syscall = 302; // prlimit64(0, resource, new_rlim, NULL)
        rule.arg_source = {{AliasArg::Kind::kConst, 0},
                           {AliasArg::Kind::kArg, 0},
                           {AliasArg::Kind::kArg, 1},
                           {AliasArg::Kind::kConst, 0}};
        rule.return_transform = kRegistryReturnTransformErrno;
        rule.rule_version = 1;
        rule.origin = RuleOrigin::kPackage;
        rule.origin_detail = "generated from the compiled-in adapter table";
        rule.enabled = true;
        rule.notes = "setrlimit(resource, rlim) is prlimit64(0, resource, rlim, NULL)";
        document.rules.push_back(std::move(rule));
    }

    return document;
}

} // namespace hyperion
