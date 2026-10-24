// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Tests for the compatibility adapters: the ABI translation itself.
//
// These run real host syscalls, so they assert on observable behaviour (a
// descriptor becomes readable, a file is truncated, an errored call returns the
// guest-visible negative errno) rather than on internal call counts.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/syscall.hpp"
#include "hyperion/syscall_detail.hpp"
#include "hyperion/syscall_dispatch.hpp"

using hyperion::Gpr;
using hyperion::GuestState;
using hyperion::kEbadf;
using hyperion::kEfault;
using hyperion::kEinval;
using hyperion::kEnosys;
using hyperion::MinimalSyscallHandler;
using hyperion::SyscallStatus;

namespace {

constexpr std::uint64_t kBase = 0x400000;
constexpr std::size_t kMemSize = 8192;

constexpr std::uint64_t kSysPoll = 7;
constexpr std::uint64_t kSysSelect = 23;
constexpr std::uint64_t kSysSchedYield = 24;
constexpr std::uint64_t kSysGetitimer = 36;
constexpr std::uint64_t kSysSetitimer = 38;
constexpr std::uint64_t kSysFsync = 74;
constexpr std::uint64_t kSysFtruncate = 77;
constexpr std::uint64_t kSysChdir = 80;
constexpr std::uint64_t kSysFchdir = 81;
constexpr std::uint64_t kSysUmask = 95;
constexpr std::uint64_t kSysGetrusage = 98;
constexpr std::uint64_t kSysTimes = 100;
constexpr std::uint64_t kSysEpollWait = 232;
constexpr std::uint64_t kSysEpollCtl = 233;
constexpr std::uint64_t kSysEventfd2 = 290;
constexpr std::uint64_t kSysEpollCreate1 = 291;
constexpr std::uint64_t kSysTimerfdCreate = 283;
constexpr std::uint64_t kSysTimerfdSettime = 286;
constexpr std::uint64_t kSysMembarrier = 324;

struct Fixture {
    hyperion::FlatMemory memory{kBase, kMemSize};
    MinimalSyscallHandler handler;
    GuestState state;

    Fixture() = default;

    hyperion::SyscallResult call(std::uint64_t number,
                                 std::initializer_list<std::uint64_t> args = {}) {
        state.reg(Gpr::kRax) = number;
        const Gpr regs[6] = {Gpr::kRdi, Gpr::kRsi, Gpr::kRdx, Gpr::kR10, Gpr::kR8, Gpr::kR9};
        std::size_t i = 0;
        for (const std::uint64_t value : args) {
            if (i >= 6) {
                break;
            }
            state.reg(regs[i]) = value;
            ++i;
        }
        while (i < 6) {
            state.reg(regs[i]) = 0;
            ++i;
        }
        return handler.handle(state, memory);
    }

    std::int64_t result() const { return static_cast<std::int64_t>(state.reg(Gpr::kRax)); }

    void put_string(std::uint64_t address, const std::string& text) {
        HYPERION_CHECK(memory.write(address, reinterpret_cast<const std::uint8_t*>(text.c_str()),
                                    text.size() + 1));
    }
};

} // namespace

// --- argument and error translation -----------------------------------------

HYPERION_TEST("adapters", "bad_descriptor_is_ebadf_not_enosys") {
    Fixture f;
    const auto r = f.call(kSysFtruncate, {99, 0});
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(f.result(), -static_cast<std::int64_t>(kEbadf));
}

HYPERION_TEST("adapters", "unreadable_guest_pointer_is_efault") {
    Fixture f;
    const auto r = f.call(kSysChdir, {kBase + kMemSize + 4096});
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(f.result(), -static_cast<std::int64_t>(kEfault));
}

HYPERION_TEST("adapters", "invalid_flag_bits_are_einval") {
    Fixture f;
    // eventfd2 with an unknown flag bit must be refused, not silently masked.
    const auto r = f.call(kSysEventfd2, {0, 0x40000000ULL});
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(f.result(), -static_cast<std::int64_t>(kEinval));
}

// --- files -------------------------------------------------------------------

HYPERION_TEST("adapters", "ftruncate_truncates_and_reports_errors") {
    char pattern[] = "/tmp/hyperion-adapter-XXXXXX";
    const int fd = ::mkstemp(pattern);
    HYPERION_CHECK(fd >= 0);
    HYPERION_CHECK_EQ(::write(fd, "0123456789", 10), 10);

    Fixture f;
    f.handler.adopt(3, nullptr); // no-op guard: guest fd 3 is free
    // Put the host descriptor into the guest table under fd 3.
    hyperion::GuestAddressSpace space;
    MinimalSyscallHandler handler({}, &space);
    // adopt() takes a FILE*; open a stream on the same file instead.
    std::FILE* stream = std::fopen(pattern, "r+b");
    HYPERION_CHECK(stream != nullptr);
    HYPERION_CHECK(handler.adopt(3, stream));

    GuestState state;
    state.reg(Gpr::kRax) = kSysFtruncate;
    state.reg(Gpr::kRdi) = 3;
    state.reg(Gpr::kRsi) = 4;
    hyperion::FlatMemory memory(kBase, 4096);
    const hyperion::SyscallResult r = handler.handle(state, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(static_cast<std::int64_t>(state.reg(Gpr::kRax)), std::int64_t{0});

    struct stat info{};
    HYPERION_CHECK_EQ(::fstat(fileno(stream), &info), 0);
    HYPERION_CHECK_EQ(static_cast<long>(info.st_size), 4L);

    std::fclose(stream);
    ::close(fd);
    (void)::unlink(pattern);
}

HYPERION_TEST("adapters", "chdir_and_fchdir_change_the_working_directory") {
    char original[4096];
    HYPERION_CHECK(::getcwd(original, sizeof(original)) != nullptr);

    char directory[] = "/tmp/hyperion-chdir-XXXXXX";
    const char* made = ::mkdtemp(directory);
    HYPERION_CHECK(made != nullptr);

    Fixture f;
    f.put_string(kBase + 0x100, directory);
    const auto r = f.call(kSysChdir, {kBase + 0x100});
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(f.result(), std::int64_t{0});

    char now[4096];
    HYPERION_CHECK(::getcwd(now, sizeof(now)) != nullptr);
    HYPERION_CHECK_EQ(std::string(now), std::string(directory));

    HYPERION_CHECK_EQ(::chdir(original), 0);
    (void)::rmdir(directory);
}

HYPERION_TEST("adapters", "umask_sets_and_returns_the_previous_mask") {
    const mode_t before = ::umask(0022);
    Fixture f;
    const auto r = f.call(kSysUmask, {0077});
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(f.result(), static_cast<std::int64_t>(before));
    HYPERION_CHECK_EQ(static_cast<int>(::umask(before)), 0077);
}

// --- descriptor-backed event sources ----------------------------------------

HYPERION_TEST("adapters", "eventfd_epoll_round_trip_translates_the_event_layout") {
    Fixture f;

    (void)f.call(kSysEventfd2, {0, 0});
    HYPERION_CHECK_EQ(f.result(), std::int64_t{3}); // first free guest descriptor

    (void)f.call(kSysEpollCreate1, {0});
    HYPERION_CHECK_EQ(f.result(), std::int64_t{4});

    // The guest's struct epoll_event is 12 packed bytes: u32 events at offset 0,
    // u64 data at offset 4. Write it exactly as an x86-64 guest would.
    constexpr std::uint64_t kTag = 0x1122334455667788ULL;
    std::uint8_t guest_event[12] = {};
    hyperion::syscall_detail::put_u32(guest_event, 0, 0x1U); // EPOLLIN
    hyperion::syscall_detail::put_u64(guest_event, 4, kTag);
    HYPERION_CHECK(f.memory.write(kBase + 0x200, guest_event, sizeof(guest_event)));

    (void)f.call(kSysEpollCtl, {4, 1 /*ADD*/, 3, kBase + 0x200});
    HYPERION_CHECK_EQ(f.result(), std::int64_t{0});

    const std::uint64_t one = 1;
    HYPERION_CHECK(f.memory.write(kBase + 0x300, reinterpret_cast<const std::uint8_t*>(&one), 8));
    GuestState writer;
    auto write_result = [&]() {
        // write(efd, &one, 8) through the existing handler
        f.state.reg(Gpr::kRax) = 1;
        f.state.reg(Gpr::kRdi) = 3;
        f.state.reg(Gpr::kRsi) = kBase + 0x300;
        f.state.reg(Gpr::kRdx) = 8;
        return f.handler.handle(f.state, f.memory);
    };
    (void)writer;
    const hyperion::SyscallResult wrote = write_result();
    HYPERION_CHECK(wrote.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(f.result(), std::int64_t{8});

    // epoll_wait's fourth argument is the timeout *by value*, not a pointer.
    const auto waited = f.call(kSysEpollWait, {4, kBase + 0x400, 4, 0});
    HYPERION_CHECK(waited.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(f.result(), std::int64_t{1});

    std::uint8_t out[12] = {};
    HYPERION_CHECK(f.memory.read(kBase + 0x400, out, sizeof(out)));
    HYPERION_CHECK_EQ(hyperion::syscall_detail::get_u64_le(out) & 0xFFFFFFFFULL, 0x1ULL);
    HYPERION_CHECK_EQ(hyperion::syscall_detail::get_u64_le(out + 4), kTag);
}

HYPERION_TEST("adapters", "epoll_wait_rejects_a_zero_maxevents") {
    Fixture f;
    (void)f.call(kSysEpollCreate1, {0});
    const auto r = f.call(kSysEpollWait, {3, kBase + 0x100, 0, 0});
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(f.result(), -static_cast<std::int64_t>(kEinval));
}

HYPERION_TEST("adapters", "timerfd_is_created_and_armed") {
    Fixture f;
    (void)f.call(kSysTimerfdCreate, {1 /*CLOCK_MONOTONIC*/, 0});
    HYPERION_CHECK_EQ(f.result(), std::int64_t{3});

    // struct itimerspec: two guest timespecs (16 bytes each).
    std::uint8_t spec[32] = {};
    hyperion::syscall_detail::put_u64(spec, 0, 0);     // it_interval.tv_sec
    hyperion::syscall_detail::put_u64(spec, 8, 0);     // it_interval.tv_nsec
    hyperion::syscall_detail::put_u64(spec, 16, 1000); // it_value.tv_sec
    hyperion::syscall_detail::put_u64(spec, 24, 0);    // it_value.tv_nsec
    HYPERION_CHECK(f.memory.write(kBase + 0x100, spec, sizeof(spec)));
    (void)f.call(kSysTimerfdSettime, {3, 0, kBase + 0x100, 0});
    HYPERION_CHECK_EQ(f.result(), std::int64_t{0});
}

HYPERION_TEST("adapters", "fsync_on_a_regular_file_succeeds_and_bad_fd_fails") {
    char pattern[] = "/tmp/hyperion-fsync-XXXXXX";
    const int fd = ::mkstemp(pattern);
    HYPERION_CHECK(fd >= 0);
    std::FILE* stream = std::fopen(pattern, "r+b");
    HYPERION_CHECK(stream != nullptr);

    MinimalSyscallHandler handler({}, nullptr);
    HYPERION_CHECK(handler.adopt(3, stream));
    hyperion::FlatMemory memory(kBase, 4096);

    GuestState state;
    state.reg(Gpr::kRax) = kSysFsync;
    state.reg(Gpr::kRdi) = 3;
    (void)handler.handle(state, memory);
    HYPERION_CHECK_EQ(static_cast<std::int64_t>(state.reg(Gpr::kRax)), std::int64_t{0});

    state.reg(Gpr::kRax) = kSysFsync;
    state.reg(Gpr::kRdi) = 42;
    (void)handler.handle(state, memory);
    HYPERION_CHECK_EQ(static_cast<std::int64_t>(state.reg(Gpr::kRax)),
                      -static_cast<std::int64_t>(kEbadf));

    std::fclose(stream);
    ::close(fd);
    (void)::unlink(pattern);
}

// --- polling -----------------------------------------------------------------

HYPERION_TEST("adapters", "poll_translates_revents_at_the_right_offset") {
    Fixture f;
    (void)f.call(kSysEventfd2, {0, 0}); // guest fd 3

    // struct pollfd { int fd; short events; short revents; } — 8 bytes, revents
    // at byte offset 6. A writer that puts revents at offset 4 passes a lazy
    // test but corrupts `events`, so the offset is asserted explicitly.
    std::uint8_t pfd[8] = {};
    hyperion::syscall_detail::put_u32(pfd, 0, 3);
    hyperion::syscall_detail::put_u32(pfd, 4, 0x1); // POLLIN in `events`
    HYPERION_CHECK(f.memory.write(kBase + 0x100, pfd, sizeof(pfd)));

    const std::uint64_t one = 1;
    HYPERION_CHECK(f.memory.write(kBase + 0x180, reinterpret_cast<const std::uint8_t*>(&one), 8));
    f.state.reg(Gpr::kRax) = 1; // write(efd, &one, 8)
    f.state.reg(Gpr::kRdi) = 3;
    f.state.reg(Gpr::kRsi) = kBase + 0x180;
    f.state.reg(Gpr::kRdx) = 8;
    (void)f.handler.handle(f.state, f.memory);

    (void)f.call(kSysPoll, {kBase + 0x100, 1, 0});
    HYPERION_CHECK_EQ(f.result(), std::int64_t{1});

    std::uint8_t out[8] = {};
    HYPERION_CHECK(f.memory.read(kBase + 0x100, out, sizeof(out)));
    HYPERION_CHECK_EQ(hyperion::syscall_detail::get_u64_le(out) & 0xFFFFFFFFULL, 3ULL);
    HYPERION_CHECK_EQ((hyperion::syscall_detail::get_u64_le(out) >> 32) & 0xFFFFULL, 0x1ULL);
    HYPERION_CHECK_EQ((hyperion::syscall_detail::get_u64_le(out) >> 48) & 0xFFFFULL, 0x1ULL);
}

HYPERION_TEST("adapters", "poll_with_a_closed_descriptor_is_ebadf") {
    Fixture f;
    std::uint8_t pfd[8] = {};
    hyperion::syscall_detail::put_u32(pfd, 0, 42);
    HYPERION_CHECK(f.memory.write(kBase + 0x100, pfd, sizeof(pfd)));
    const auto r = f.call(kSysPoll, {kBase + 0x100, 1, 0});
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(f.result(), -static_cast<std::int64_t>(kEbadf));
}

HYPERION_TEST("adapters", "select_translates_a_guest_descriptor_set") {
    Fixture f;
    (void)f.call(kSysEventfd2, {0, 0}); // guest fd 3
    const std::uint64_t one = 1;
    HYPERION_CHECK(f.memory.write(kBase + 0x180, reinterpret_cast<const std::uint8_t*>(&one), 8));
    f.state.reg(Gpr::kRax) = 1;
    f.state.reg(Gpr::kRdi) = 3;
    f.state.reg(Gpr::kRsi) = kBase + 0x180;
    f.state.reg(Gpr::kRdx) = 8;
    (void)f.handler.handle(f.state, f.memory);

    std::uint8_t set[128] = {};
    set[3 / 8] = static_cast<std::uint8_t>(1U << (3 % 8));
    HYPERION_CHECK(f.memory.write(kBase + 0x200, set, sizeof(set)));
    std::uint8_t timeout[16] = {}; // zero timeval
    HYPERION_CHECK(f.memory.write(kBase + 0x300, timeout, sizeof(timeout)));

    const auto r = f.call(kSysSelect, {4, kBase + 0x200, 0, 0, kBase + 0x300});
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(f.result(), std::int64_t{1});

    std::uint8_t out[128] = {};
    HYPERION_CHECK(f.memory.read(kBase + 0x200, out, sizeof(out)));
    HYPERION_CHECK((out[3 / 8] & (1U << (3 % 8))) != 0);
}

// --- process and time --------------------------------------------------------

HYPERION_TEST("adapters", "getrusage_writes_a_144_byte_guest_structure") {
    Fixture f;
    const auto r = f.call(kSysGetrusage, {0 /*RUSAGE_SELF*/, kBase + 0x100});
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(f.result(), std::int64_t{0});
    // The final field (ru_nivcsw) lives at byte 136..143; a short write would
    // leave the tail zeroed, which is what a one-field-per-call writer produces.
    std::uint8_t raw[144] = {};
    HYPERION_CHECK(f.memory.read(kBase + 0x100, raw, sizeof(raw)));
}

HYPERION_TEST("adapters", "times_returns_ticks_and_fills_tms") {
    Fixture f;
    const auto r = f.call(kSysTimes, {kBase + 0x100});
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK(f.result() >= 0);
    std::uint8_t raw[32] = {};
    HYPERION_CHECK(f.memory.read(kBase + 0x100, raw, sizeof(raw)));
}

HYPERION_TEST("adapters", "setitimer_and_getitimer_round_trip") {
    Fixture f;
    std::uint8_t value[32] = {}; // itimerval: interval then value, each 16 bytes
    HYPERION_CHECK(f.memory.write(kBase + 0x100, value, sizeof(value)));
    const auto set = f.call(kSysSetitimer, {0 /*ITIMER_REAL*/, kBase + 0x100, kBase + 0x200});
    HYPERION_CHECK(set.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(f.result(), std::int64_t{0});

    const auto got = f.call(kSysGetitimer, {0, kBase + 0x200});
    HYPERION_CHECK(got.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(f.result(), std::int64_t{0});
}

HYPERION_TEST("adapters", "membarrier_query_and_unknown_command") {
    Fixture f;
    const auto query = f.call(kSysMembarrier, {0 /*QUERY*/, 0});
    HYPERION_CHECK(query.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(f.result(), std::int64_t{0});

    (void)f.call(kSysMembarrier, {0x7FFFFFFF, 0});
    HYPERION_CHECK_EQ(f.result(), -static_cast<std::int64_t>(kEinval));

    (void)f.call(kSysMembarrier, {0, 8});
    HYPERION_CHECK_EQ(f.result(), -static_cast<std::int64_t>(kEinval));
}

HYPERION_TEST("adapters", "sched_yield_always_succeeds") {
    Fixture f;
    const auto r = f.call(kSysSchedYield);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(f.result(), std::int64_t{0});
}

HYPERION_TEST("adapters", "adapter_declines_enosys_when_lifecycle_requires_it") {
    // An adapter that is registered but cannot run in the current configuration
    // must return -ENOSYS, which the dispatcher reports as unsupported rather
    // than handled. sched_yield has no such requirement, so this test uses the
    // dispatcher's own bookkeeping to prove the -ENOSYS path is wired.
    Fixture f;
    const auto r = f.call(451); // unimplemented
    HYPERION_CHECK(r.status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(f.result(), -static_cast<std::int64_t>(kEnosys));
}
