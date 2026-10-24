// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest syscall layer tests (Phase 6, increment 1).
//
// These verify the `syscall` instruction, the handler dispatch and the
// write/exit/exit_group semantics against a real Memory and GuestState, with a
// capturing output sink so nothing leaks into the test runner's stdout. The
// tests that require executing generated AArch64 code are guarded on
// __aarch64__, matching the rest of the suite.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

#include "hyperion/syscall_detail.hpp"

#include "harness/test_harness.hpp"
#include "hyperion/address_space.hpp"
#include "hyperion/decoder.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/run.hpp"
#include "hyperion/syscall.hpp"
#include "hyperion/translation_cache.hpp"

using hyperion::FlatMemory;
using hyperion::Gpr;
using hyperion::GuestAddressSpace;
using hyperion::GuestState;
using hyperion::MemPerm;
using hyperion::MinimalSyscallHandler;
using hyperion::Opcode;
using hyperion::RunStatus;
using hyperion::StepStatus;
using hyperion::SyscallStatus;

namespace {

constexpr std::uint64_t kBase = 0x400000;
constexpr std::size_t kMemSize = 4096;

// Captured writes, keyed by fd.
struct Captured {
    std::string out;
    std::string err;
};

MinimalSyscallHandler::OutputSink sink_to(Captured& cap) {
    return [&cap](int fd, const std::uint8_t* data, std::size_t count) -> long {
        std::string& dst = (fd == 2) ? cap.err : cap.out;
        dst.append(reinterpret_cast<const char*>(data), count);
        return static_cast<long>(count);
    };
}

// A tiny guest program builder: place bytes at kBase.
FlatMemory program(const std::vector<std::uint8_t>& code) {
    FlatMemory memory(kBase, kMemSize);
    std::memcpy(memory.data(), code.data(), code.size());
    return memory;
}

} // namespace

// --- Decoding --------------------------------------------------------------

HYPERION_TEST("syscall", "decodes_0f_05_as_syscall") {
    const std::uint8_t code[] = {0x0F, 0x05};
    const hyperion::DecodeResult r = hyperion::decode(code, sizeof(code), kBase);
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.opcode == Opcode::kSyscall);
    HYPERION_CHECK_EQ(r.instruction.length, std::uint8_t{2});
}

HYPERION_TEST("syscall", "truncated_0f_is_truncated") {
    const std::uint8_t code[] = {0x0F};
    const hyperion::DecodeResult r = hyperion::decode(code, sizeof(code), kBase);
    HYPERION_CHECK(!r.ok());
    HYPERION_CHECK(r.error == hyperion::DecodeError::kTruncated);
}

HYPERION_TEST("syscall", "unknown_0f_escape_is_rejected") {
    const std::uint8_t code[] = {0x0F, 0x0B}; // UD2: outside the subset
    const hyperion::DecodeResult r = hyperion::decode(code, sizeof(code), kBase);
    HYPERION_CHECK(!r.ok());
    HYPERION_CHECK(r.error == hyperion::DecodeError::kUnknownOpcode);
}

// --- Handler semantics (no generated code needed) --------------------------

HYPERION_TEST("syscall", "write_copies_guest_buffer_to_sink") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));

    FlatMemory memory = program({});
    const char* msg = "hello";
    HYPERION_CHECK(memory.write(kBase + 0x100, reinterpret_cast<const std::uint8_t*>(msg), 5));

    GuestState s;
    s.reg(Gpr::kRax) = hyperion::sysno::kWrite;
    s.reg(Gpr::kRdi) = 1;             // fd 1
    s.reg(Gpr::kRsi) = kBase + 0x100; // buf
    s.reg(Gpr::kRdx) = 5;             // count

    const hyperion::SyscallResult r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(cap.out, std::string("hello"));
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{5}); // bytes written
}

HYPERION_TEST("syscall", "write_to_bad_fd_is_ebadf") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    FlatMemory memory = program({});

    GuestState s;
    s.reg(Gpr::kRax) = hyperion::sysno::kWrite;
    s.reg(Gpr::kRdi) = 7; // not 1 or 2
    s.reg(Gpr::kRsi) = kBase + 0x100;
    s.reg(Gpr::kRdx) = 1;

    const hyperion::SyscallResult r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEbadf));
    HYPERION_CHECK(cap.out.empty());
}

HYPERION_TEST("syscall", "write_out_of_range_buffer_is_efault") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    FlatMemory memory = program({});

    GuestState s;
    s.reg(Gpr::kRax) = hyperion::sysno::kWrite;
    s.reg(Gpr::kRdi) = 1;
    s.reg(Gpr::kRsi) = kBase + kMemSize + 4096; // unmapped
    s.reg(Gpr::kRdx) = 4;

    const hyperion::SyscallResult r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));
    HYPERION_CHECK(cap.out.empty());
}

HYPERION_TEST("syscall", "exit_reports_code_and_does_not_touch_rax_result") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    FlatMemory memory = program({});

    GuestState s;
    s.reg(Gpr::kRax) = hyperion::sysno::kExit;
    s.reg(Gpr::kRdi) = 42;

    const hyperion::SyscallResult r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kExit);
    HYPERION_CHECK_EQ(r.exit_code, std::int64_t{42});
}

HYPERION_TEST("syscall", "unknown_number_is_enosys_not_silent") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    FlatMemory memory = program({});

    GuestState s;
    s.reg(Gpr::kRax) = 202; // futex: not implemented in this increment

    const hyperion::SyscallResult r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(r.number, std::uint64_t{202});
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
    HYPERION_CHECK(!hyperion::is_minimal_syscall(202));
}

// --- Interpreter integration ----------------------------------------------

HYPERION_TEST("interpreter", "syscall_write_then_exit_halts_program") {
    // mov eax, 1 (write) ; mov edi, 1 ; mov esi, buf ; mov edx, 5 ;
    // syscall ; mov eax, 60 (exit) ; mov edi, 0 ; syscall ; hlt
    // The buffer is placed in the same flat memory at kBase + 0x200.
    std::vector<std::uint8_t> code = {
        0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
        0xBF, 0x01, 0x00, 0x00, 0x00, // mov edi, 1
        0xBE, 0x00, 0x02, 0x40, 0x00, // mov esi, 0x400200
        0xBA, 0x05, 0x00, 0x00, 0x00, // mov edx, 5
        0x0F, 0x05,                   // syscall
        0xB8, 0x3C, 0x00, 0x00, 0x00, // mov eax, 60
        0x31, 0xFF,                   // xor edi, edi
        0x0F, 0x05,                   // syscall (exit)
        0xF4,                         // hlt
    };
    FlatMemory memory = program(code);
    const char* msg = "hyper";
    HYPERION_CHECK(memory.write(kBase + 0x200, reinterpret_cast<const std::uint8_t*>(msg), 5));

    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));

    GuestState initial;
    initial.rip = kBase;
    const hyperion::RunResult r = hyperion::run_program(memory, initial, 100, &handler);

    HYPERION_CHECK(r.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(cap.out, std::string("hyper"));
    HYPERION_CHECK_EQ(r.unimplemented_syscalls, std::uint64_t{0});
}

HYPERION_TEST("interpreter", "syscall_without_handler_is_unsupported_not_ignored") {
    const std::vector<std::uint8_t> code = {0x0F, 0x05, 0xF4};
    FlatMemory memory = program(code);
    GuestState initial;
    initial.rip = kBase;

    const hyperion::RunResult r = hyperion::run_program(memory, initial, 10);
    HYPERION_CHECK(r.status == RunStatus::kUnsupported);
}

HYPERION_TEST("interpreter", "unimplemented_syscall_returns_enosys_and_continues") {
    // mov eax, 202 (futex) ; syscall ; hlt. futex is unimplemented, so rax must
    // end as -ENOSYS and the program must continue to the hlt and stop cleanly.
    const std::vector<std::uint8_t> code = {
        0xB8, 0xCA, 0x00, 0x00, 0x00, // mov eax, 202
        0x0F, 0x05,                   // syscall
        0xF4,                         // hlt
    };
    FlatMemory memory = program(code);
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    GuestState initial;
    initial.rip = kBase;

    const hyperion::RunResult r = hyperion::run_program(memory, initial, 10, &handler);
    HYPERION_CHECK(r.status == RunStatus::kOk); // hlt reached
    HYPERION_CHECK_EQ(r.unimplemented_syscalls, std::uint64_t{1});
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
}

// --- Address-space syscalls (mmap/mprotect/munmap/brk) ----------------------

// Build a guest state with the syscall number in rax and up to six arguments in
// the x86-64 syscall argument registers.
GuestState syscall_state(std::uint64_t number, std::initializer_list<std::uint64_t> args) {
    GuestState s;
    s.reg(Gpr::kRax) = number;
    const Gpr regs[6] = {Gpr::kRdi, Gpr::kRsi, Gpr::kRdx, Gpr::kR10, Gpr::kR8, Gpr::kR9};
    std::size_t i = 0;
    for (std::uint64_t a : args) {
        if (i < 6) {
            s.reg(regs[i]) = a;
        }
        ++i;
    }
    return s;
}

HYPERION_TEST("syscall", "mmap_anonymous_returns_a_mapping_that_is_usable") {
    GuestAddressSpace as;
    MinimalSyscallHandler handler({}, &as);
    FlatMemory memory(kBase, kMemSize);

    // mmap(NULL, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    GuestState s =
        syscall_state(hyperion::sysno::kMmap,
                      {0, 8192, hyperion::mmap_flags::kProtRead | hyperion::mmap_flags::kProtWrite,
                       hyperion::mmap_flags::kMapPrivate | hyperion::mmap_flags::kMapAnonymous,
                       static_cast<std::uint64_t>(-1), 0});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);

    const std::uint64_t addr = s.reg(Gpr::kRax);
    HYPERION_CHECK((addr & (GuestAddressSpace::kPageSize - 1)) == 0); // page-aligned
    const std::uint8_t v = 0x99;
    HYPERION_CHECK(as.write(addr, &v, 1));
    std::uint8_t out = 0;
    HYPERION_CHECK(as.read(addr, &out, 1));
    HYPERION_CHECK(out == 0x99);
}

HYPERION_TEST("syscall", "mmap_zero_length_is_einval") {
    GuestAddressSpace as;
    MinimalSyscallHandler handler({}, &as);
    FlatMemory memory(kBase, kMemSize);
    GuestState s = syscall_state(
        hyperion::sysno::kMmap,
        {0, 0, hyperion::mmap_flags::kProtRead,
         hyperion::mmap_flags::kMapPrivate | hyperion::mmap_flags::kMapAnonymous, 0, 0});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEinval));
}

HYPERION_TEST("syscall", "mmap_oversized_request_is_enomem_not_a_host_termination") {
    // Security regression: the mapping's backing store is host memory sized by
    // a guest-controlled value. An enormous anonymous mmap must be refused with
    // -ENOMEM, not allowed to throw std::bad_alloc (which would terminate the
    // host process) or commit gigabytes.
    GuestAddressSpace as;
    MinimalSyscallHandler handler({}, &as);
    FlatMemory memory(kBase, kMemSize);
    const std::uint64_t huge = 1ULL << 60; // far beyond the per-mapping cap
    GuestState s = syscall_state(
        hyperion::sysno::kMmap,
        {0, huge, hyperion::mmap_flags::kProtRead | hyperion::mmap_flags::kProtWrite,
         hyperion::mmap_flags::kMapPrivate | hyperion::mmap_flags::kMapAnonymous, 0, 0});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled); // handled, with a negative errno
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnomem));
}

HYPERION_TEST("syscall", "mmap_file_backed_is_enosys_not_a_wrong_mapping") {
    GuestAddressSpace as;
    MinimalSyscallHandler handler({}, &as);
    FlatMemory memory(kBase, kMemSize);
    // No MAP_ANONYMOUS: a file-backed mapping, which this increment does not do.
    GuestState s = syscall_state(hyperion::sysno::kMmap, {0, 4096, hyperion::mmap_flags::kProtRead,
                                                          hyperion::mmap_flags::kMapPrivate, 3, 0});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
}

HYPERION_TEST("syscall", "mprotect_and_munmap_round_trip") {
    GuestAddressSpace as;
    MinimalSyscallHandler handler({}, &as);
    FlatMemory memory(kBase, kMemSize);

    GuestState m = syscall_state(
        hyperion::sysno::kMmap,
        {0, 4096, hyperion::mmap_flags::kProtRead | hyperion::mmap_flags::kProtWrite,
         hyperion::mmap_flags::kMapPrivate | hyperion::mmap_flags::kMapAnonymous, 0, 0});
    handler.handle(m, memory);
    const std::uint64_t addr = m.reg(Gpr::kRax);
    HYPERION_CHECK(addr != 0);

    // mprotect to read-only: a write through the address space must then fail.
    GuestState p =
        syscall_state(hyperion::sysno::kMprotect, {addr, 4096, hyperion::mmap_flags::kProtRead});
    HYPERION_CHECK(handler.handle(p, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(p.reg(Gpr::kRax), std::uint64_t{0});
    const std::uint8_t v = 1;
    HYPERION_CHECK(!as.write(addr, &v, 1));

    // munmap removes it entirely.
    GuestState u = syscall_state(hyperion::sysno::kMunmap, {addr, 4096});
    HYPERION_CHECK(handler.handle(u, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(u.reg(Gpr::kRax), std::uint64_t{0});
    std::uint8_t out = 0;
    HYPERION_CHECK(!as.read(addr, &out, 1));
}

HYPERION_TEST("syscall", "brk_returns_the_new_break") {
    GuestAddressSpace as(0x7f0000000000ULL, 0x200000ULL, 1ULL << 20);
    MinimalSyscallHandler handler({}, &as);
    FlatMemory memory(kBase, kMemSize);

    GuestState s = syscall_state(hyperion::sysno::kBrk, {0x201000});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{0x201000});
    const std::uint8_t v = 0x5A;
    HYPERION_CHECK(as.write(0x200FF0, &v, 1)); // the grown heap is writable
}

HYPERION_TEST("syscall", "memory_syscalls_without_an_address_space_are_enosys") {
    MinimalSyscallHandler handler; // no address space
    FlatMemory memory(kBase, kMemSize);
    GuestState s = syscall_state(
        hyperion::sysno::kMmap,
        {0, 4096, hyperion::mmap_flags::kProtRead,
         hyperion::mmap_flags::kMapPrivate | hyperion::mmap_flags::kMapAnonymous, 0, 0});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
}

HYPERION_TEST("syscall", "is_minimal_syscall_reports_the_new_set") {
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kRead));
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kWrite));
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kClose));
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kLseek));
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kMmap));
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kMprotect));
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kMunmap));
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kBrk));
    HYPERION_CHECK(!hyperion::is_minimal_syscall(2));   // open, still unimplemented
    HYPERION_CHECK(!hyperion::is_minimal_syscall(202)); // futex, still unimplemented
}

// --- File-descriptor syscalls (read/close/lseek) ---------------------------
//
// These use adopt() to attach an already-open host stream under a guest fd, so
// the test needs no real file on disk and no open(2). The handler must never
// fclose an adopted stream: the test owns it.

// A temporary in-memory-ish stream: fmemopen is a POSIX stream over a buffer,
// which is seekable and has a definite end, so read/close/lseek can be tested
// without touching the host filesystem.
struct MemStream {
    std::FILE* f = nullptr;
    explicit MemStream(const char* data, std::size_t size) {
        f = ::fmemopen(const_cast<char*>(data), size, "r");
    }
    ~MemStream() {
        if (f != nullptr) {
            std::fclose(f);
        }
    }
    MemStream(const MemStream&) = delete;
    MemStream& operator=(const MemStream&) = delete;
};

HYPERION_TEST("syscall", "read_copies_from_adopted_stream_into_guest_memory") {
    static const char kData[] = "hyperion";
    MemStream ms(kData, sizeof(kData) - 1);
    HYPERION_CHECK(ms.f != nullptr);

    MinimalSyscallHandler handler;
    HYPERION_CHECK(handler.adopt(3, ms.f)); // guest fd 3 -> the stream

    FlatMemory memory = program({});
    GuestState s = syscall_state(hyperion::sysno::kRead, {3, kBase + 0x200, 8});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{8}); // all 8 bytes read

    std::uint8_t got[8] = {};
    HYPERION_CHECK(memory.read(kBase + 0x200, got, 8));
    HYPERION_CHECK(std::memcmp(got, "hyperion", 8) == 0);
}

HYPERION_TEST("syscall", "read_of_bad_fd_is_ebadf") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    GuestState s = syscall_state(hyperion::sysno::kRead, {7, kBase + 0x200, 4});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEbadf));
}

HYPERION_TEST("syscall", "read_out_of_range_buffer_is_efault") {
    static const char kData[] = "hyperion";
    MemStream ms(kData, sizeof(kData) - 1);
    MinimalSyscallHandler handler;
    HYPERION_CHECK(handler.adopt(3, ms.f));

    FlatMemory memory = program({});
    GuestState s = syscall_state(hyperion::sysno::kRead, {3, kBase + kMemSize + 4096, 4});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));
}

HYPERION_TEST("syscall", "read_short_read_at_end_of_stream") {
    static const char kData[] = "abc";
    MemStream ms(kData, sizeof(kData) - 1);
    MinimalSyscallHandler handler;
    HYPERION_CHECK(handler.adopt(3, ms.f));

    FlatMemory memory = program({});
    // Ask for more than the stream holds: read(2) returns a short count, not an
    // error.
    GuestState s = syscall_state(hyperion::sysno::kRead, {3, kBase + 0x200, 100});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{3});
}

HYPERION_TEST("syscall", "close_drops_the_descriptor_but_not_the_stream") {
    static const char kData[] = "hyperion";
    MemStream ms(kData, sizeof(kData) - 1);
    MinimalSyscallHandler handler;
    HYPERION_CHECK(handler.adopt(3, ms.f));

    FlatMemory memory = program({});
    GuestState c = syscall_state(hyperion::sysno::kClose, {3});
    HYPERION_CHECK(handler.handle(c, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(c.reg(Gpr::kRax), std::uint64_t{0});

    // The descriptor is gone: a second close, or a read, is -EBADF.
    GuestState c2 = syscall_state(hyperion::sysno::kClose, {3});
    handler.handle(c2, memory);
    HYPERION_CHECK_EQ(c2.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEbadf));

    // The caller still owns the stream and can use it.
    char buf[4] = {};
    HYPERION_CHECK(std::fread(buf, 1, 3, ms.f) == 3);
    HYPERION_CHECK(std::memcmp(buf, "hyp", 3) == 0);
}

HYPERION_TEST("syscall", "close_of_bad_fd_is_ebadf") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    GuestState s = syscall_state(hyperion::sysno::kClose, {42});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEbadf));
}

HYPERION_TEST("syscall", "lseek_repositions_an_adopted_stream") {
    static const char kData[] = "hyperion";
    MemStream ms(kData, sizeof(kData) - 1);
    MinimalSyscallHandler handler;
    HYPERION_CHECK(handler.adopt(3, ms.f));

    FlatMemory memory = program({});
    // Seek to offset 2 from the start: the next read sees "perion".
    GuestState s = syscall_state(hyperion::sysno::kLseek, {3, 2, hyperion::seek::kSet});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{2});

    GuestState rd = syscall_state(hyperion::sysno::kRead, {3, kBase + 0x300, 6});
    handler.handle(rd, memory);
    HYPERION_CHECK_EQ(rd.reg(Gpr::kRax), std::uint64_t{6});
    std::uint8_t got[6] = {};
    HYPERION_CHECK(memory.read(kBase + 0x300, got, 6));
    HYPERION_CHECK(std::memcmp(got, "perion", 6) == 0);
}

HYPERION_TEST("syscall", "lseek_end_and_cur_whence_work") {
    static const char kData[] = "hyperion";
    MemStream ms(kData, sizeof(kData) - 1);
    MinimalSyscallHandler handler;
    HYPERION_CHECK(handler.adopt(3, ms.f));

    FlatMemory memory = program({});
    GuestState e = syscall_state(hyperion::sysno::kLseek, {3, 0, hyperion::seek::kEnd});
    handler.handle(e, memory);
    HYPERION_CHECK_EQ(e.reg(Gpr::kRax), std::uint64_t{8}); // size of the stream

    GuestState c = syscall_state(hyperion::sysno::kLseek,
                                 {3, static_cast<std::uint64_t>(-2), hyperion::seek::kCur});
    handler.handle(c, memory);
    HYPERION_CHECK_EQ(c.reg(Gpr::kRax), std::uint64_t{6});
}

HYPERION_TEST("syscall", "lseek_bad_whence_is_einval_and_bad_fd_is_ebadf") {
    static const char kData[] = "hyperion";
    MemStream ms(kData, sizeof(kData) - 1);
    MinimalSyscallHandler handler;
    HYPERION_CHECK(handler.adopt(3, ms.f));

    FlatMemory memory = program({});
    GuestState w = syscall_state(hyperion::sysno::kLseek, {3, 0, 99});
    handler.handle(w, memory);
    HYPERION_CHECK_EQ(w.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEinval));

    GuestState b = syscall_state(hyperion::sysno::kLseek, {9, 0, hyperion::seek::kSet});
    handler.handle(b, memory);
    HYPERION_CHECK_EQ(b.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEbadf));
}

HYPERION_TEST("syscall", "adopt_refuses_an_occupied_descriptor") {
    MinimalSyscallHandler handler; // fd 0/1/2 are the standard streams
    HYPERION_CHECK(!handler.adopt(0, stdin));
    HYPERION_CHECK(!handler.adopt(-1, stdin));
    HYPERION_CHECK(!handler.adopt(3, nullptr));
}

HYPERION_TEST("syscall", "open_legacy_number_is_still_unimplemented") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    // open(2) (number 2) is not implemented; openat(2) (257) is. A guest that
    // uses the legacy number gets -ENOSYS, not a silently wrong result.
    GuestState o = syscall_state(2, {kBase + 0x200, 0, 0});
    const auto r = handler.handle(o, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(o.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
}

// --- openat / fstat (Phase 6, fourth slice) --------------------------------
//
// These exercise the real host filesystem, because openat() forwards to the
// host kernel (SECURITY.md: Hyperion is not a sandbox). A temporary file is
// created with mkstemp so the test is self-contained and cleans up after itself.

// A temporary file path, removed on destruction.
struct TempFile {
    std::string path;
    int fd = -1;
    TempFile() {
        char tmpl[] = "/tmp/hyperion_openat_XXXXXX";
        fd = ::mkstemp(tmpl);
        if (fd >= 0) {
            path = tmpl;
        }
    }
    ~TempFile() {
        if (fd >= 0) {
            ::close(fd);
        }
        if (!path.empty()) {
            ::unlink(path.c_str());
        }
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

// Copy a C string into guest memory at `addr`.
void put_cstr(FlatMemory& memory, std::uint64_t addr, const char* text) {
    const std::size_t n = std::strlen(text) + 1;
    HYPERION_CHECK(memory.write(addr, reinterpret_cast<const std::uint8_t*>(text), n));
}

std::uint64_t read_u64(FlatMemory& memory, std::uint64_t addr) {
    std::uint8_t b[8];
    HYPERION_CHECK(memory.read(addr, b, 8));
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(b[static_cast<std::size_t>(i)]) << (8 * i);
    }
    return v;
}

std::uint32_t read_u32(FlatMemory& memory, std::uint64_t addr) {
    std::uint8_t b[4];
    HYPERION_CHECK(memory.read(addr, b, 4));
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(b[static_cast<std::size_t>(i)]) << (8 * i);
    }
    return v;
}

HYPERION_TEST("syscall", "openat_opens_a_real_file_and_returns_a_guest_fd") {
    TempFile tf;
    HYPERION_CHECK(tf.fd >= 0);
    HYPERION_CHECK(::write(tf.fd, "hyperion\n", 9) == 9);

    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, tf.path.c_str());

    GuestState s = syscall_state(hyperion::sysno::kOpenat,
                                 {hyperion::kAtFdcwd, kBase + 0x400, 0 /*O_RDONLY*/, 0});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    const std::int64_t gfd = static_cast<std::int64_t>(s.reg(Gpr::kRax));
    HYPERION_CHECK(gfd >= 3); // 0/1/2 are taken; the lowest free slot is 3

    // The descriptor is real: read the file back through it.
    GuestState rd =
        syscall_state(hyperion::sysno::kRead, {static_cast<std::uint64_t>(gfd), kBase + 0x600, 9});
    HYPERION_CHECK(handler.handle(rd, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(rd.reg(Gpr::kRax), std::uint64_t{9});
    std::uint8_t got[9] = {};
    HYPERION_CHECK(memory.read(kBase + 0x600, got, 9));
    HYPERION_CHECK_EQ(std::memcmp(got, "hyperion\n", 9), 0);
}

HYPERION_TEST("syscall", "openat_missing_file_is_enoent") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, "/tmp/hyperion_definitely_does_not_exist_9c1f");

    GuestState s =
        syscall_state(hyperion::sysno::kOpenat, {hyperion::kAtFdcwd, kBase + 0x400, 0, 0});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnoent));
}

HYPERION_TEST("syscall", "openat_bad_path_pointer_is_efault") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    // A pointer outside the mapped region: the pathname read must fail cleanly.
    GuestState s =
        syscall_state(hyperion::sysno::kOpenat, {hyperion::kAtFdcwd, 0xDEADBEEF0000ULL, 0, 0});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));
}

HYPERION_TEST("syscall", "openat_with_a_real_dirfd_is_unimplemented") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, ".");
    // dirfd = 3 (not AT_FDCWD): a real *at() is not implemented, so the call is
    // reported unimplemented rather than resolved against the wrong base.
    GuestState s = syscall_state(hyperion::sysno::kOpenat, {3, kBase + 0x400, 0, 0});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
}

HYPERION_TEST("syscall", "openat_unhandled_flag_is_unimplemented") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, ".");
    // O_NOFOLLOW (0x20000) is outside the honoured set: not silently dropped.
    // (O_DIRECTORY/O_NONBLOCK/O_CLOEXEC are honoured as of the getdents64 slice.)
    GuestState s =
        syscall_state(hyperion::sysno::kOpenat, {hyperion::kAtFdcwd, kBase + 0x400, 0x20000, 0});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
}

HYPERION_TEST("syscall", "openat_writes_and_closes_release_the_guest_fd") {
    TempFile tf;
    HYPERION_CHECK(tf.fd >= 0);
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, tf.path.c_str());

    GuestState o =
        syscall_state(hyperion::sysno::kOpenat, {hyperion::kAtFdcwd, kBase + 0x400, 0, 0});
    HYPERION_CHECK(handler.handle(o, memory).status == SyscallStatus::kHandled);
    const std::int64_t gfd = static_cast<std::int64_t>(o.reg(Gpr::kRax));
    HYPERION_CHECK(gfd >= 3);

    // Close it: the descriptor is gone (a second close is -EBADF) and the slot
    // is reused by the next open.
    GuestState c = syscall_state(hyperion::sysno::kClose, {static_cast<std::uint64_t>(gfd)});
    HYPERION_CHECK(handler.handle(c, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(c.reg(Gpr::kRax), std::uint64_t{0});

    GuestState c2 = syscall_state(hyperion::sysno::kClose, {static_cast<std::uint64_t>(gfd)});
    handler.handle(c2, memory);
    HYPERION_CHECK_EQ(c2.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEbadf));

    GuestState o2 =
        syscall_state(hyperion::sysno::kOpenat, {hyperion::kAtFdcwd, kBase + 0x400, 0, 0});
    handler.handle(o2, memory);
    HYPERION_CHECK_EQ(static_cast<std::int64_t>(o2.reg(Gpr::kRax)), gfd); // slot reused
}

HYPERION_TEST("syscall", "fstat_reports_size_in_the_guest_layout") {
    TempFile tf;
    HYPERION_CHECK(tf.fd >= 0);
    HYPERION_CHECK(::write(tf.fd, "0123456789", 10) == 10);

    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, tf.path.c_str());
    GuestState o =
        syscall_state(hyperion::sysno::kOpenat, {hyperion::kAtFdcwd, kBase + 0x400, 0, 0});
    HYPERION_CHECK(handler.handle(o, memory).status == SyscallStatus::kHandled);
    const std::uint64_t gfd = o.reg(Gpr::kRax);

    GuestState fs = syscall_state(hyperion::sysno::kFstat, {gfd, kBase + 0x800});
    HYPERION_CHECK(handler.handle(fs, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(fs.reg(Gpr::kRax), std::uint64_t{0});

    // st_size is at offset 48 in the x86-64 struct stat. This is the ABI-sensitive
    // check: the host layout differs on AArch64, so a memcpy of the host struct
    // would put the wrong value here.
    HYPERION_CHECK_EQ(read_u64(memory, kBase + 0x800 + hyperion::guest_stat::kOffStSize),
                      std::uint64_t{10});
    // st_mode at 24: a regular file has S_IFREG (0x8000) in the top bits.
    const std::uint32_t mode = read_u32(memory, kBase + 0x800 + hyperion::guest_stat::kOffStMode);
    HYPERION_CHECK((mode & 0xF000u) == 0x8000u);
}

HYPERION_TEST("syscall", "fstat_bad_fd_is_ebadf_and_bad_buffer_is_efault") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    GuestState s = syscall_state(hyperion::sysno::kFstat, {99, kBase + 0x800});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEbadf));

    // A valid fd (stdin) but an unmapped stat buffer: -EFAULT.
    GuestState s2 = syscall_state(hyperion::sysno::kFstat, {0, 0xDEADBEEF0000ULL});
    handler.handle(s2, memory);
    HYPERION_CHECK_EQ(s2.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));
}

HYPERION_TEST("syscall", "openat_is_advertised_as_minimal") {
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kOpenat));
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kFstat));
    HYPERION_CHECK(!hyperion::is_minimal_syscall(257 + 1000));
}

// --- newfstatat (path-based stat, fifth/sixth slice) ------------------------

HYPERION_TEST("syscall", "newfstatat_reports_path_metadata_in_the_guest_layout") {
    TempFile tf;
    HYPERION_CHECK(tf.fd >= 0);
    HYPERION_CHECK(::write(tf.fd, "0123456789", 10) == 10);

    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, tf.path.c_str());

    GuestState s = syscall_state(hyperion::sysno::kNewfstatat,
                                 {hyperion::kAtFdcwd, kBase + 0x400, kBase + 0x800, 0});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{0});

    // Same ABI-sensitive check as fstat: st_size at offset 48, st_mode at 24.
    HYPERION_CHECK_EQ(read_u64(memory, kBase + 0x800 + hyperion::guest_stat::kOffStSize),
                      std::uint64_t{10});
    const std::uint32_t mode = read_u32(memory, kBase + 0x800 + hyperion::guest_stat::kOffStMode);
    HYPERION_CHECK((mode & 0xF000u) == 0x8000u);
}

HYPERION_TEST("syscall", "newfstatat_missing_path_is_enoent") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, "/nonexistent/hyperion/path/xyzzy");

    GuestState s = syscall_state(hyperion::sysno::kNewfstatat,
                                 {hyperion::kAtFdcwd, kBase + 0x400, kBase + 0x800, 0});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnoent));
}

HYPERION_TEST("syscall", "newfstatat_bad_path_pointer_is_efault") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    GuestState s = syscall_state(hyperion::sysno::kNewfstatat,
                                 {hyperion::kAtFdcwd, 0xDEADBEEF0000ULL, kBase + 0x800, 0});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));
}

HYPERION_TEST("syscall", "newfstatat_unmapped_buffer_is_efault") {
    TempFile tf;
    HYPERION_CHECK(tf.fd >= 0);

    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, tf.path.c_str());

    GuestState s = syscall_state(hyperion::sysno::kNewfstatat,
                                 {hyperion::kAtFdcwd, kBase + 0x400, 0xDEADBEEF0000ULL, 0});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));
}

HYPERION_TEST("syscall", "newfstatat_with_a_real_dirfd_or_bad_flag_is_unimplemented") {
    TempFile tf;
    HYPERION_CHECK(tf.fd >= 0);

    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, tf.path.c_str());

    // A real directory descriptor is not supported: -ENOSYS, counted as
    // unimplemented rather than resolved against the wrong base.
    GuestState dir =
        syscall_state(hyperion::sysno::kNewfstatat, {3, kBase + 0x400, kBase + 0x800, 0});
    HYPERION_CHECK(handler.handle(dir, memory).status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(dir.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));

    // AT_EMPTY_PATH (0x1000) is outside the honoured set.
    GuestState flag = syscall_state(hyperion::sysno::kNewfstatat,
                                    {hyperion::kAtFdcwd, kBase + 0x400, kBase + 0x800, 0x1000});
    HYPERION_CHECK(handler.handle(flag, memory).status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(flag.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
}

HYPERION_TEST("syscall", "newfstatat_symlink_nofollow_uses_lstat") {
    // A dangling symlink resolves with stat (follow -> ENOENT) but reports the
    // link itself with AT_SYMLINK_NOFOLLOW (lstat -> S_IFLNK).
    char link_path[] = "/tmp/hyperion_nfstat_link_XXXXXX";
    const int fd = ::mkstemp(link_path);
    HYPERION_CHECK(fd >= 0);
    ::close(fd);
    ::unlink(link_path); // remove the temp file, keep the unique name
    HYPERION_CHECK(::symlink("/nonexistent/hyperion/target", link_path) == 0);

    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, link_path);

    GuestState follow = syscall_state(hyperion::sysno::kNewfstatat,
                                      {hyperion::kAtFdcwd, kBase + 0x400, kBase + 0x800, 0});
    handler.handle(follow, memory);
    HYPERION_CHECK_EQ(follow.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnoent));

    GuestState nofollow =
        syscall_state(hyperion::sysno::kNewfstatat, {hyperion::kAtFdcwd, kBase + 0x400,
                                                     kBase + 0x800, hyperion::kAtSymlinkNofollow});
    HYPERION_CHECK(handler.handle(nofollow, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(nofollow.reg(Gpr::kRax), std::uint64_t{0});
    const std::uint32_t mode = read_u32(memory, kBase + 0x800 + hyperion::guest_stat::kOffStMode);
    HYPERION_CHECK((mode & 0xF000u) == 0xA000u); // S_IFLNK

    ::unlink(link_path);
}

HYPERION_TEST("syscall", "newfstatat_is_advertised_as_minimal") {
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kNewfstatat));
    HYPERION_CHECK(!hyperion::is_minimal_syscall(hyperion::sysno::kNewfstatat + 1000));
}

// --- access(21) and readlink(89) (seventh slice) ---------------------------

HYPERION_TEST("syscall", "access_reports_a_real_file_and_enoent_for_a_missing_one") {
    TempFile tf;
    HYPERION_CHECK(tf.fd >= 0);

    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, tf.path.c_str());

    GuestState ok = syscall_state(hyperion::sysno::kAccess, {kBase + 0x400, 0 /*F_OK*/});
    HYPERION_CHECK(handler.handle(ok, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(ok.reg(Gpr::kRax), std::uint64_t{0});

    put_cstr(memory, kBase + 0x500, "/nonexistent/hyperion/path/xyzzy");
    GuestState missing = syscall_state(hyperion::sysno::kAccess, {kBase + 0x500, 0});
    handler.handle(missing, memory);
    HYPERION_CHECK_EQ(missing.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnoent));
}

HYPERION_TEST("syscall", "access_bad_mode_is_einval_and_bad_path_pointer_is_efault") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, "/tmp");

    // Bit 0x8 is not a valid access mode.
    GuestState bad = syscall_state(hyperion::sysno::kAccess, {kBase + 0x400, 0x8});
    handler.handle(bad, memory);
    HYPERION_CHECK_EQ(bad.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEinval));

    GuestState fault = syscall_state(hyperion::sysno::kAccess, {0xDEADBEEF0000ULL, 0});
    handler.handle(fault, memory);
    HYPERION_CHECK_EQ(fault.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));
}

HYPERION_TEST("syscall", "readlink_copies_the_target_without_a_terminator") {
    char link_path[] = "/tmp/hyperion_readlink_XXXXXX";
    const int fd = ::mkstemp(link_path);
    HYPERION_CHECK(fd >= 0);
    ::close(fd);
    ::unlink(link_path);
    HYPERION_CHECK(::symlink("/tmp/hyperion/target", link_path) == 0);

    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, link_path);

    GuestState s = syscall_state(hyperion::sysno::kReadlink, {kBase + 0x400, kBase + 0x800, 64});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    const char* expected = "/tmp/hyperion/target";
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(std::strlen(expected)));

    std::uint8_t got[64] = {};
    HYPERION_CHECK(memory.read(kBase + 0x800, got, std::strlen(expected)));
    HYPERION_CHECK_EQ(
        std::strncmp(reinterpret_cast<const char*>(got), expected, std::strlen(expected)), 0);

    ::unlink(link_path);
}

HYPERION_TEST("syscall", "readlink_truncates_a_long_target_and_rejects_zero_size") {
    char link_path[] = "/tmp/hyperion_readlink2_XXXXXX";
    const int fd = ::mkstemp(link_path);
    HYPERION_CHECK(fd >= 0);
    ::close(fd);
    ::unlink(link_path);
    HYPERION_CHECK(::symlink("/tmp/hyperion/target", link_path) == 0);

    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, link_path);

    // A short buffer truncates to its size, and that is not an error.
    GuestState trunc = syscall_state(hyperion::sysno::kReadlink, {kBase + 0x400, kBase + 0x800, 4});
    HYPERION_CHECK(handler.handle(trunc, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(trunc.reg(Gpr::kRax), std::uint64_t{4});

    // bufsiz 0 is EINVAL, as readlink(2) documents.
    GuestState zero = syscall_state(hyperion::sysno::kReadlink, {kBase + 0x400, kBase + 0x800, 0});
    handler.handle(zero, memory);
    HYPERION_CHECK_EQ(zero.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEinval));

    // A non-symlink path is EINVAL on Linux.
    TempFile plain;
    HYPERION_CHECK(plain.fd >= 0);
    put_cstr(memory, kBase + 0x500, plain.path.c_str());
    GuestState notlink =
        syscall_state(hyperion::sysno::kReadlink, {kBase + 0x500, kBase + 0x800, 64});
    handler.handle(notlink, memory);
    HYPERION_CHECK_EQ(notlink.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEinval));

    ::unlink(link_path);
}

HYPERION_TEST("syscall", "access_and_readlink_are_advertised_as_minimal") {
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kAccess));
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kReadlink));
    HYPERION_CHECK(!hyperion::is_minimal_syscall(hyperion::sysno::kAccess + 1000));
}

// --- readlinkat(267) (eighth slice) ----------------------------------------

HYPERION_TEST("syscall", "readlinkat_with_at_fdcwd_matches_readlink") {
    char link_path[] = "/tmp/hyperion_readlinkat_XXXXXX";
    const int fd = ::mkstemp(link_path);
    HYPERION_CHECK(fd >= 0);
    ::close(fd);
    ::unlink(link_path);
    HYPERION_CHECK(::symlink("/tmp/hyperion/at_target", link_path) == 0);

    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, link_path);

    GuestState s = syscall_state(hyperion::sysno::kReadlinkat,
                                 {hyperion::kAtFdcwd, kBase + 0x400, kBase + 0x800, 64});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    const char* expected = "/tmp/hyperion/at_target";
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(std::strlen(expected)));

    std::uint8_t got[64] = {};
    HYPERION_CHECK(memory.read(kBase + 0x800, got, std::strlen(expected)));
    HYPERION_CHECK_EQ(
        std::strncmp(reinterpret_cast<const char*>(got), expected, std::strlen(expected)), 0);

    ::unlink(link_path);
}

HYPERION_TEST("syscall", "readlinkat_with_a_real_dirfd_is_unimplemented") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, "/tmp");

    // dirfd 3 is a real descriptor, not AT_FDCWD: outside this increment.
    GuestState s =
        syscall_state(hyperion::sysno::kReadlinkat, {3, kBase + 0x400, kBase + 0x800, 64});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
}

HYPERION_TEST("syscall", "readlinkat_rejects_zero_size_and_bad_pointer") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, "/tmp");

    GuestState zero = syscall_state(hyperion::sysno::kReadlinkat,
                                    {hyperion::kAtFdcwd, kBase + 0x400, kBase + 0x800, 0});
    handler.handle(zero, memory);
    HYPERION_CHECK_EQ(zero.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEinval));

    GuestState fault = syscall_state(hyperion::sysno::kReadlinkat,
                                     {hyperion::kAtFdcwd, 0xDEADBEEF0000ULL, kBase + 0x800, 64});
    handler.handle(fault, memory);
    HYPERION_CHECK_EQ(fault.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));
}

HYPERION_TEST("syscall", "readlinkat_is_advertised_as_minimal") {
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kReadlinkat));
}

// --- getdents64 (ninth slice) ----------------------------------------------

// A temporary directory with one known file inside, removed on destruction.
struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/hyperion_getdents_XXXXXX";
        char* made = ::mkdtemp(tmpl);
        if (made != nullptr) {
            path = made;
            const std::string child = path + "/known_entry";
            const int fd = ::open(child.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
            if (fd >= 0) {
                ::close(fd);
            }
        }
    }
    ~TempDir() {
        if (!path.empty()) {
            ::unlink((path + "/known_entry").c_str());
            ::rmdir(path.c_str());
        }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// Parse the linux_dirent64 records in `buf` and return their names.
std::vector<std::string> parse_dirent_names(const std::uint8_t* buf, std::size_t len) {
    std::vector<std::string> names;
    std::size_t off = 0;
    while (off + 24 <= len) {
        std::uint16_t reclen = 0;
        std::memcpy(&reclen, buf + off + 16, sizeof(reclen));
        if (reclen < 24 || off + reclen > len) {
            break; // malformed; stop rather than read out of bounds
        }
        const char* name = reinterpret_cast<const char*>(buf + off + 19);
        names.emplace_back(name, ::strnlen(name, reclen - 19));
        off += reclen;
    }
    return names;
}

HYPERION_TEST("syscall", "getdents64_lists_a_directory_through_openat") {
    TempDir dir;
    HYPERION_CHECK(!dir.path.empty());
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, dir.path.c_str());

    // opendir-style open: O_RDONLY | O_NONBLOCK | O_DIRECTORY. These flags must
    // now be honoured, not reported unimplemented.
    const std::uint64_t flags = hyperion::open_flags::kORdonly | hyperion::open_flags::kONonblock |
                                hyperion::open_flags::kODirectory;
    GuestState o =
        syscall_state(hyperion::sysno::kOpenat, {hyperion::kAtFdcwd, kBase + 0x400, flags, 0});
    HYPERION_CHECK(handler.handle(o, memory).status == SyscallStatus::kHandled);
    const std::int64_t gfd = static_cast<std::int64_t>(o.reg(Gpr::kRax));
    HYPERION_CHECK(gfd >= 3);

    GuestState g = syscall_state(hyperion::sysno::kGetdents64,
                                 {static_cast<std::uint64_t>(gfd), kBase + 0x800, 4096});
    const hyperion::SyscallResult r = handler.handle(g, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    const std::int64_t got = static_cast<std::int64_t>(g.reg(Gpr::kRax));
    HYPERION_CHECK(got > 0);

    std::vector<std::uint8_t> buf(static_cast<std::size_t>(got));
    HYPERION_CHECK(memory.read(kBase + 0x800, buf.data(), buf.size()));
    const std::vector<std::string> names = parse_dirent_names(buf.data(), buf.size());
    HYPERION_CHECK(std::find(names.begin(), names.end(), ".") != names.end());
    HYPERION_CHECK(std::find(names.begin(), names.end(), "..") != names.end());
    HYPERION_CHECK(std::find(names.begin(), names.end(), "known_entry") != names.end());
}

HYPERION_TEST("syscall", "getdents64_on_a_regular_file_is_enotdir") {
    TempFile tf;
    HYPERION_CHECK(tf.fd >= 0);
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, tf.path.c_str());

    GuestState o = syscall_state(hyperion::sysno::kOpenat, {hyperion::kAtFdcwd, kBase + 0x400,
                                                            hyperion::open_flags::kORdonly, 0});
    HYPERION_CHECK(handler.handle(o, memory).status == SyscallStatus::kHandled);
    const std::int64_t gfd = static_cast<std::int64_t>(o.reg(Gpr::kRax));

    GuestState g = syscall_state(hyperion::sysno::kGetdents64,
                                 {static_cast<std::uint64_t>(gfd), kBase + 0x800, 4096});
    HYPERION_CHECK(handler.handle(g, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(g.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnotdir));
}

HYPERION_TEST("syscall", "getdents64_bad_fd_is_ebadf_and_small_count_is_einval") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});

    GuestState bad = syscall_state(hyperion::sysno::kGetdents64, {42, kBase + 0x800, 4096});
    handler.handle(bad, memory);
    HYPERION_CHECK_EQ(bad.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEbadf));

    TempDir dir;
    HYPERION_CHECK(!dir.path.empty());
    put_cstr(memory, kBase + 0x400, dir.path.c_str());
    GuestState o = syscall_state(hyperion::sysno::kOpenat, {hyperion::kAtFdcwd, kBase + 0x400,
                                                            hyperion::open_flags::kODirectory, 0});
    HYPERION_CHECK(handler.handle(o, memory).status == SyscallStatus::kHandled);
    const std::int64_t gfd = static_cast<std::int64_t>(o.reg(Gpr::kRax));

    // A buffer smaller than one dirent (24 bytes) is EINVAL, as the kernel says.
    GuestState small = syscall_state(hyperion::sysno::kGetdents64,
                                     {static_cast<std::uint64_t>(gfd), kBase + 0x800, 20});
    handler.handle(small, memory);
    HYPERION_CHECK_EQ(small.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEinval));
}

HYPERION_TEST("syscall", "getdents64_bad_buffer_is_efault") {
    TempDir dir;
    HYPERION_CHECK(!dir.path.empty());
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, dir.path.c_str());

    GuestState o = syscall_state(hyperion::sysno::kOpenat, {hyperion::kAtFdcwd, kBase + 0x400,
                                                            hyperion::open_flags::kODirectory, 0});
    HYPERION_CHECK(handler.handle(o, memory).status == SyscallStatus::kHandled);
    const std::int64_t gfd = static_cast<std::int64_t>(o.reg(Gpr::kRax));

    // A valid directory but an unmapped destination buffer.
    GuestState g = syscall_state(hyperion::sysno::kGetdents64,
                                 {static_cast<std::uint64_t>(gfd), kBase + kMemSize + 4096, 4096});
    handler.handle(g, memory);
    HYPERION_CHECK_EQ(g.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));
}

HYPERION_TEST("syscall", "getdents64_is_advertised_as_minimal") {
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kGetdents64));
}

HYPERION_TEST("syscall", "openat_accepts_the_directory_flags") {
    TempDir dir;
    HYPERION_CHECK(!dir.path.empty());
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, dir.path.c_str());

    // O_DIRECTORY | O_NONBLOCK | O_CLOEXEC are now honoured, so a directory
    // opens as a handled call rather than being reported unimplemented.
    const std::uint64_t flags = hyperion::open_flags::kORdonly | hyperion::open_flags::kONonblock |
                                hyperion::open_flags::kODirectory |
                                hyperion::open_flags::kOCloseexec;
    GuestState s =
        syscall_state(hyperion::sysno::kOpenat, {hyperion::kAtFdcwd, kBase + 0x400, flags, 0});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK(static_cast<std::int64_t>(s.reg(Gpr::kRax)) >= 3);
}

// --- statx(332) (tenth slice) ----------------------------------------------

// Read a 16-bit little-endian field from guest memory.
std::uint16_t read_u16(FlatMemory& memory, std::uint64_t addr) {
    std::uint8_t b[2];
    HYPERION_CHECK(memory.read(addr, b, 2));
    return static_cast<std::uint16_t>(b[0] | (static_cast<std::uint16_t>(b[1]) << 8));
}

HYPERION_TEST("syscall", "statx_reports_a_real_file_in_the_guest_layout") {
    TempFile tf;
    HYPERION_CHECK(tf.fd >= 0);
    HYPERION_CHECK(::write(tf.fd, "hyperion\n", 9) == 9);

    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, tf.path.c_str());

    GuestState s =
        syscall_state(hyperion::sysno::kStatx, {hyperion::kAtFdcwd, kBase + 0x400, 0,
                                                hyperion::statx::kBasicStats, kBase + 0x800});
    const hyperion::SyscallResult r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{0});

    // The size is the field a test can assert without a second stat source.
    HYPERION_CHECK_EQ(read_u64(memory, kBase + 0x800 + hyperion::guest_statx::kOffSize),
                      std::uint64_t{9});
    // stx_mode is S_IFREG | 0600; the file-type bits must be S_IFREG.
    HYPERION_CHECK_EQ(read_u16(memory, kBase + 0x800 + hyperion::guest_statx::kOffMode) & 0xF000,
                      static_cast<std::uint16_t>(0x8000));
}

HYPERION_TEST("syscall", "statx_missing_path_is_enoent") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, "/no/such/hyperion/path");

    GuestState s =
        syscall_state(hyperion::sysno::kStatx, {hyperion::kAtFdcwd, kBase + 0x400, 0,
                                                hyperion::statx::kBasicStats, kBase + 0x800});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnoent));
}

HYPERION_TEST("syscall", "statx_bad_buffer_is_efault_and_bad_pointer_is_efault") {
    TempFile tf;
    HYPERION_CHECK(tf.fd >= 0);
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, tf.path.c_str());

    // A valid path but an unmapped destination buffer.
    GuestState badbuf = syscall_state(hyperion::sysno::kStatx,
                                      {hyperion::kAtFdcwd, kBase + 0x400, 0,
                                       hyperion::statx::kBasicStats, kBase + kMemSize + 4096});
    handler.handle(badbuf, memory);
    HYPERION_CHECK_EQ(badbuf.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));

    // An unmapped pathname pointer.
    GuestState badpath =
        syscall_state(hyperion::sysno::kStatx, {hyperion::kAtFdcwd, 0xDEADBEEF0000ULL, 0,
                                                hyperion::statx::kBasicStats, kBase + 0x800});
    handler.handle(badpath, memory);
    HYPERION_CHECK_EQ(badpath.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));
}

HYPERION_TEST("syscall", "statx_with_a_real_dirfd_is_unimplemented") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, "/tmp");

    GuestState s =
        syscall_state(hyperion::sysno::kStatx,
                      {3, kBase + 0x400, 0, hyperion::statx::kBasicStats, kBase + 0x800});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
}

HYPERION_TEST("syscall", "statx_unhandled_flag_is_unimplemented") {
    MinimalSyscallHandler handler;
    FlatMemory memory = program({});
    put_cstr(memory, kBase + 0x400, "/tmp");

    // AT_EMPTY_PATH (0x1000) is outside the honoured set.
    GuestState s = syscall_state(hyperion::sysno::kStatx,
                                 {hyperion::kAtFdcwd, kBase + 0x400, hyperion::statx::kAtEmptyPath,
                                  hyperion::statx::kBasicStats, kBase + 0x800});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
}

HYPERION_TEST("syscall", "statx_is_advertised_as_minimal") {
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kStatx));
}

// --- Process-identity getters, getcwd, getrandom (fifth slice) -------------

HYPERION_TEST("syscall", "identity_getters_forward_to_the_host") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    FlatMemory memory = program({});

    struct Case {
        std::uint64_t number;
        long expected;
    };
    const Case cases[] = {
        {hyperion::sysno::kGetpid, ::getpid()},   {hyperion::sysno::kGettid, ::gettid()},
        {hyperion::sysno::kGetuid, ::getuid()},   {hyperion::sysno::kGeteuid, ::geteuid()},
        {hyperion::sysno::kGetgid, ::getgid()},   {hyperion::sysno::kGetegid, ::getegid()},
        {hyperion::sysno::kGetppid, ::getppid()},
    };
    for (const Case& c : cases) {
        GuestState s = syscall_state(c.number, {});
        const hyperion::SyscallResult r = handler.handle(s, memory);
        HYPERION_CHECK(r.status == SyscallStatus::kHandled);
        HYPERION_CHECK_EQ(static_cast<long>(s.reg(Gpr::kRax)), c.expected);
    }
}

HYPERION_TEST("syscall", "getcwd_copies_the_host_working_directory") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    FlatMemory memory = program({});

    char expected[4096];
    HYPERION_CHECK(::getcwd(expected, sizeof(expected)) != nullptr);
    const std::size_t len = std::strlen(expected);

    GuestState s = syscall_state(hyperion::sysno::kGetcwd, {kBase + 0x100, 4096});
    const hyperion::SyscallResult r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(len + 1));

    // The guest buffer holds the path plus its NUL terminator.
    std::uint8_t got[4096] = {};
    HYPERION_CHECK(memory.read(kBase + 0x100, got, len + 1));
    HYPERION_CHECK_EQ(std::strcmp(reinterpret_cast<const char*>(got), expected), 0);
}

HYPERION_TEST("syscall", "getcwd_too_small_is_erange_and_bad_buffer_is_efault") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    FlatMemory memory = program({});

    // size 1 cannot hold even a one-character path plus its NUL.
    GuestState small = syscall_state(hyperion::sysno::kGetcwd, {kBase + 0x100, 1});
    handler.handle(small, memory);
    HYPERION_CHECK_EQ(small.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kErange));

    // size 0 is EINVAL, as getcwd(2) documents.
    GuestState zero = syscall_state(hyperion::sysno::kGetcwd, {kBase + 0x100, 0});
    handler.handle(zero, memory);
    HYPERION_CHECK_EQ(zero.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEinval));

    // An unmapped destination is EFAULT, not a host write.
    GuestState bad = syscall_state(hyperion::sysno::kGetcwd, {0xDEADBEEF0000ULL, 4096});
    handler.handle(bad, memory);
    HYPERION_CHECK_EQ(bad.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));
}

HYPERION_TEST("syscall", "getrandom_fills_the_guest_buffer") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    FlatMemory memory = program({});

    // Zero the destination first so a no-op cannot pass by accident.
    const std::uint8_t zeros[16] = {};
    HYPERION_CHECK(memory.write(kBase + 0x100, zeros, sizeof(zeros)));

    GuestState s = syscall_state(hyperion::sysno::kGetrandom, {kBase + 0x100, 16, 0});
    const hyperion::SyscallResult r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{16});

    std::uint8_t got[16] = {};
    HYPERION_CHECK(memory.read(kBase + 0x100, got, sizeof(got)));
    // 128 random bits being all zero has probability 2^-128; treat any nonzero
    // byte as proof the buffer was filled.
    bool any = false;
    for (std::uint8_t b : got) {
        any = any || (b != 0);
    }
    HYPERION_CHECK(any);
}

HYPERION_TEST("syscall", "getrandom_zero_length_is_zero_and_bad_flags_is_einval") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    FlatMemory memory = program({});

    GuestState zero = syscall_state(hyperion::sysno::kGetrandom, {kBase + 0x100, 0, 0});
    handler.handle(zero, memory);
    HYPERION_CHECK_EQ(zero.reg(Gpr::kRax), std::uint64_t{0});

    // Bit 0x4 is not a defined GRND_* flag.
    GuestState bad = syscall_state(hyperion::sysno::kGetrandom, {kBase + 0x100, 8, 0x4});
    handler.handle(bad, memory);
    HYPERION_CHECK_EQ(bad.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEinval));

    // An unmapped destination is EFAULT.
    GuestState fault = syscall_state(hyperion::sysno::kGetrandom, {0xDEADBEEF0000ULL, 8, 0});
    handler.handle(fault, memory);
    HYPERION_CHECK_EQ(fault.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));
}

HYPERION_TEST("syscall", "identity_getters_are_advertised_as_minimal") {
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kGetpid));
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kGettid));
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kGetcwd));
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kGetrandom));
    HYPERION_CHECK(!hyperion::is_minimal_syscall(hyperion::sysno::kGetppid + 1000));
}

// A guest program that mmaps a page and stores a byte through the returned
// pointer, exercising the whole path: the syscall handler allocates in the
// GuestAddressSpace, the guest writes to it with a normal memory store, and the
// interpreter reads code from the same address space. This is the integration
// the unit tests above do not cover (they call the handler directly).
//
// Only encodings in the current subset are used (mov r32, imm32; mov r/m, r;
// xor; syscall; hlt). The mapping is requested MAP_FIXED at 0x50000000, an
// address that fits in 32 bits, so the guest's 32-bit `mov ebx, eax` preserves
// the pointer the handler returned and the store goes to the right place.
//
//   mov eax, 9        ; mmap
//   mov edi, 0x50000000 ; addr = MAP_FIXED hint
//   mov esi, 4096     ; length
//   mov edx, 3        ; PROT_READ|PROT_WRITE
//   mov r10d, 0x32    ; MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED
//   mov r8d, -1       ; fd
//   xor r9d, r9d      ; offset
//   syscall
//   mov ebx, eax      ; save the returned address
//   mov ecx, 0xAB
//   mov [rbx], ecx    ; store through the mapping
//   hlt
std::vector<std::uint8_t> mmap_program() {
    return {
        0xB8, 0x09, 0x00, 0x00, 0x00,       // mov eax, 9
        0xBF, 0x00, 0x00, 0x00, 0x50,       // mov edi, 0x50000000
        0xBE, 0x00, 0x10, 0x00, 0x00,       // mov esi, 4096
        0xBA, 0x03, 0x00, 0x00, 0x00,       // mov edx, 3
        0x41, 0xBA, 0x32, 0x00, 0x00, 0x00, // mov r10d, 0x32
        0x41, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, // mov r8d, -1
        0x45, 0x31, 0xC9,                   // xor r9d, r9d
        0x0F, 0x05,                         // syscall
        0x89, 0xC3,                         // mov ebx, eax
        0xB9, 0xAB, 0x00, 0x00, 0x00,       // mov ecx, 0xAB
        0x89, 0x0B,                         // mov [rbx], ecx
        0xF4,                               // hlt
    };
}

// Map the program image into `as` at kBase and run it, returning the result.
hyperion::RunResult run_mmap_program(GuestAddressSpace& as, Captured& cap, bool use_cache) {
    const std::vector<std::uint8_t> code = mmap_program();
    HYPERION_CHECK(as.map_image(kBase, code.data(), code.size(), MemPerm::kRead | MemPerm::kExec));

    MinimalSyscallHandler handler(sink_to(cap), &as);
    GuestState initial;
    initial.rip = kBase;
    if (use_cache) {
        hyperion::TranslationCache cache;
        return cache.run(as, initial, 200, &handler);
    }
    return hyperion::run_program(as, initial, 200, &handler);
}

HYPERION_TEST("syscall", "mmap_end_to_end_under_the_interpreter") {
    GuestAddressSpace as;
    Captured cap;
    const hyperion::RunResult r = run_mmap_program(as, cap, /*use_cache=*/false);

    HYPERION_CHECK(r.status == RunStatus::kOk);                    // hlt reached
    HYPERION_CHECK_EQ(r.unimplemented_syscalls, std::uint64_t{0}); // mmap was handled
    const std::uint64_t addr = r.state.reg(Gpr::kRbx);
    HYPERION_CHECK((addr & (GuestAddressSpace::kPageSize - 1)) == 0);
    std::uint8_t stored = 0;
    HYPERION_CHECK(as.read(addr, &stored, 1));
    HYPERION_CHECK(stored == 0xAB); // the guest's store landed in the mapping
}

#if defined(__aarch64__)
HYPERION_TEST("syscall", "mmap_end_to_end_under_generated_code_stores_through_mapping") {
    GuestAddressSpace as;
    Captured cap;
    const hyperion::RunResult r = run_mmap_program(as, cap, /*use_cache=*/true);

    HYPERION_CHECK(r.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(r.unimplemented_syscalls, std::uint64_t{0});
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRbx), std::uint64_t{0x50000000});
    std::uint8_t stored = 0;
    HYPERION_CHECK(as.read(0x50000000, &stored, 1));
    HYPERION_CHECK(stored == 0xAB);
}
#else
HYPERION_TEST("syscall", "mmap_end_to_end_under_generated_code_stores_through_mapping") {
    HYPERION_SKIP("host is not AArch64; cannot execute generated AArch64 code");
}
#endif // __aarch64__

#if defined(__aarch64__)
HYPERION_TEST("translation_cache", "tier1_syscall_write_matches_interpreter") {
    std::vector<std::uint8_t> code = {
        0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
        0xBF, 0x01, 0x00, 0x00, 0x00, // mov edi, 1
        0xBE, 0x00, 0x02, 0x40, 0x00, // mov esi, 0x400200
        0xBA, 0x05, 0x00, 0x00, 0x00, // mov edx, 5
        0x0F, 0x05,                   // syscall
        0xB8, 0x3C, 0x00, 0x00, 0x00, // mov eax, 60
        0x31, 0xFF,                   // xor edi, edi
        0x0F, 0x05,                   // syscall (exit)
        0xF4,                         // hlt
    };
    FlatMemory memory = program(code);
    const char* msg = "hyper";
    HYPERION_CHECK(memory.write(kBase + 0x200, reinterpret_cast<const std::uint8_t*>(msg), 5));

    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    GuestState initial;
    initial.rip = kBase;

    hyperion::TranslationCache cache;
    const hyperion::RunResult r = cache.run(memory, initial, 100, &handler);

    HYPERION_CHECK(r.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(cap.out, std::string("hyper"));
}
#else
HYPERION_TEST("translation_cache", "tier1_syscall_write_matches_interpreter") {
    HYPERION_SKIP("host is not AArch64; cannot execute generated AArch64 code");
}
#endif // __aarch64__

HYPERION_TEST("syscall", "zero_extended_32bit_at_fdcwd_and_unbuffered_rdwr_file_io") {
    char tmpl[] = "/tmp/hyperion_syscall_XXXXXX";
    const int pre_fd = ::mkstemp(tmpl);
    HYPERION_CHECK(pre_fd >= 0);
    ::close(pre_fd);

    FlatMemory memory(kBase, 8192);
    const std::string p(tmpl);
    HYPERION_CHECK(memory.write(kBase + 0x100, reinterpret_cast<const std::uint8_t*>(p.c_str()),
                                p.size() + 1));
    const char payload[] = "unbuffered-rdwr!";
    HYPERION_CHECK(memory.write(kBase + 0x200, reinterpret_cast<const std::uint8_t*>(payload),
                                sizeof(payload) - 1));

    MinimalSyscallHandler handler;

    // Crucial real-program ABI test: `mov edi, -100` sets RDI = 0x00000000ffffff9c
    // (upper 32 bits zeroed!). Open with O_RDWR | O_TRUNC.
    GuestState s;
    s.reg(Gpr::kRax) = hyperion::sysno::kOpenat;
    s.reg(Gpr::kRdi) = 0x00000000FFFFFF9CULL; // zero-extended AT_FDCWD (-100)
    s.reg(Gpr::kRsi) = kBase + 0x100;
    s.reg(Gpr::kRdx) = hyperion::open_flags::kORdwr | hyperion::open_flags::kOTrunc;
    s.reg(Gpr::kR10) = 0600;
    const auto open_res = handler.handle(s, memory);
    HYPERION_CHECK(open_res.status == SyscallStatus::kHandled);
    const auto guest_fd = static_cast<std::int64_t>(s.reg(Gpr::kRax));
    HYPERION_CHECK(guest_fd >= 3);

    // Write directly to the opened regular file via guest `write(fd, ...)`.
    s = GuestState{};
    s.reg(Gpr::kRax) = hyperion::sysno::kWrite;
    s.reg(Gpr::kRdi) = static_cast<std::uint64_t>(guest_fd);
    s.reg(Gpr::kRsi) = kBase + 0x200;
    s.reg(Gpr::kRdx) = sizeof(payload) - 1;
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), sizeof(payload) - 1);

    // Seek back to byte 0 and read back through guest `read(fd, ...)`.
    s = GuestState{};
    s.reg(Gpr::kRax) = hyperion::sysno::kLseek;
    s.reg(Gpr::kRdi) = static_cast<std::uint64_t>(guest_fd);
    s.reg(Gpr::kRsi) = 0;
    s.reg(Gpr::kRdx) = hyperion::seek::kSet;
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), 0ULL);

    s = GuestState{};
    s.reg(Gpr::kRax) = hyperion::sysno::kRead;
    s.reg(Gpr::kRdi) = static_cast<std::uint64_t>(guest_fd);
    s.reg(Gpr::kRsi) = kBase + 0x300;
    s.reg(Gpr::kRdx) = sizeof(payload) - 1;
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), sizeof(payload) - 1);

    char readback[sizeof(payload)] = {};
    HYPERION_CHECK(
        memory.read(kBase + 0x300, reinterpret_cast<std::uint8_t*>(readback), sizeof(payload) - 1));
    HYPERION_CHECK_EQ(std::string(readback), std::string(payload));

    // Close stdout (fd 1) and verify subsequent write(1, ...) returns -EBADF.
    s = GuestState{};
    s.reg(Gpr::kRax) = hyperion::sysno::kClose;
    s.reg(Gpr::kRdi) = 1;
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), 0ULL);

    s = GuestState{};
    s.reg(Gpr::kRax) = hyperion::sysno::kWrite;
    s.reg(Gpr::kRdi) = 1;
    s.reg(Gpr::kRsi) = kBase + 0x200;
    s.reg(Gpr::kRdx) = 4;
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(static_cast<std::int64_t>(s.reg(Gpr::kRax)), -hyperion::kEbadf);

    ::unlink(tmpl);
}

HYPERION_TEST("syscall", "phase6_extended_syscalls_dup_fcntl_iov_pread_uname_clock_and_tls") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    FlatMemory memory(kBase, 8192);

    // 1. arch_prctl ARCH_SET_FS / ARCH_GET_FS + set_tid_address + set_robust_list
    GuestState s = syscall_state(hyperion::sysno::kArchPrctl,
                                 {hyperion::arch_prctl_code::kSetFs, 0x700012340000ULL});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), 0ULL);
    HYPERION_CHECK_EQ(s.fs_base, 0x700012340000ULL);

    s.reg(Gpr::kRax) = hyperion::sysno::kArchPrctl;
    s.reg(Gpr::kRdi) = hyperion::arch_prctl_code::kGetFs;
    s.reg(Gpr::kRsi) = kBase + 0x100;
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(read_u64(memory, kBase + 0x100), 0x700012340000ULL);

    GuestState tid_s = syscall_state(hyperion::sysno::kSetTidAddress, {kBase + 0x108});
    HYPERION_CHECK(handler.handle(tid_s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK(tid_s.reg(Gpr::kRax) > 0);

    GuestState bad_rl = syscall_state(hyperion::sysno::kSetRobustList, {kBase + 0x110, 16});
    HYPERION_CHECK(handler.handle(bad_rl, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(static_cast<std::int64_t>(bad_rl.reg(Gpr::kRax)), -hyperion::kEinval);

    GuestState ok_rl = syscall_state(hyperion::sysno::kSetRobustList, {kBase + 0x110, 24});
    HYPERION_CHECK(handler.handle(ok_rl, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(ok_rl.reg(Gpr::kRax), 0ULL);

    // 2. uname always advertises "Linux" + guest machine "x86_64"
    GuestState un = syscall_state(hyperion::sysno::kUname, {kBase + 0x200});
    HYPERION_CHECK(handler.handle(un, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(un.reg(Gpr::kRax), 0ULL);
    char sysname[65] = {};
    char machine[65] = {};
    HYPERION_CHECK(memory.read(kBase + 0x200, reinterpret_cast<std::uint8_t*>(sysname), 65));
    HYPERION_CHECK(memory.read(kBase + 0x200 + 260, reinterpret_cast<std::uint8_t*>(machine), 65));
    HYPERION_CHECK_EQ(std::string(sysname), std::string("Linux"));
    HYPERION_CHECK_EQ(std::string(machine), std::string("x86_64"));

    // 3. clock_gettime + gettimeofday + prlimit64
    GuestState clk =
        syscall_state(hyperion::sysno::kClockGettime, {1 /*CLOCK_MONOTONIC*/, kBase + 0x400});
    HYPERION_CHECK(handler.handle(clk, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(clk.reg(Gpr::kRax), 0ULL);

    GuestState gtod = syscall_state(hyperion::sysno::kGettimeofday, {kBase + 0x420, kBase + 0x440});
    HYPERION_CHECK(handler.handle(gtod, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(gtod.reg(Gpr::kRax), 0ULL);

    GuestState rlim =
        syscall_state(hyperion::sysno::kPrlimit64, {0, 3 /*RLIMIT_STACK*/, 0, kBase + 0x460});
    HYPERION_CHECK(handler.handle(rlim, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(rlim.reg(Gpr::kRax), 0ULL);
    HYPERION_CHECK(read_u64(memory, kBase + 0x460) > 0);

    // 4. writev on stdout + dup/dup2/dup3/fcntl/pwrite64/pread64/readv on a real file
    const char part1[] = "Hello, ";
    const char part2[] = "writev!\n";
    HYPERION_CHECK(memory.write(kBase + 0x500, reinterpret_cast<const std::uint8_t*>(part1), 7));
    HYPERION_CHECK(memory.write(kBase + 0x510, reinterpret_cast<const std::uint8_t*>(part2), 8));
    auto write_u64 = [&](std::uint64_t addr, std::uint64_t v) {
        std::uint8_t b[8];
        for (int i = 0; i < 8; ++i) {
            b[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        }
        HYPERION_CHECK(memory.write(addr, b, 8));
    };
    write_u64(kBase + 0x540, kBase + 0x500);
    write_u64(kBase + 0x548, 7);
    write_u64(kBase + 0x550, kBase + 0x510);
    write_u64(kBase + 0x558, 8);
    GuestState wv = syscall_state(hyperion::sysno::kWritev, {1, kBase + 0x540, 2});
    HYPERION_CHECK(handler.handle(wv, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(wv.reg(Gpr::kRax), 15ULL);
    HYPERION_CHECK_EQ(cap.out, std::string("Hello, writev!\n"));

    TempFile tf;
    HYPERION_CHECK(tf.fd >= 0);
    put_cstr(memory, kBase + 0x600, tf.path.c_str());
    GuestState op = syscall_state(hyperion::sysno::kOpenat, {hyperion::kAtFdcwd, kBase + 0x600,
                                                             hyperion::open_flags::kORdwr, 0});
    HYPERION_CHECK(handler.handle(op, memory).status == SyscallStatus::kHandled);
    const std::uint64_t fd_a = op.reg(Gpr::kRax);

    GuestState d1 = syscall_state(hyperion::sysno::kDup, {fd_a});
    HYPERION_CHECK(handler.handle(d1, memory).status == SyscallStatus::kHandled);
    const std::uint64_t fd_b = d1.reg(Gpr::kRax);
    HYPERION_CHECK(fd_b != fd_a);

    GuestState d3 =
        syscall_state(hyperion::sysno::kDup3, {fd_a, 10, hyperion::open_flags::kOCloseexec});
    HYPERION_CHECK(handler.handle(d3, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(d3.reg(Gpr::kRax), 10ULL);

    GuestState gf_a =
        syscall_state(hyperion::sysno::kFcntl, {fd_a, hyperion::fcntl_cmd::kGetfd, 0});
    GuestState gf_10 = syscall_state(hyperion::sysno::kFcntl, {10, hyperion::fcntl_cmd::kGetfd, 0});
    HYPERION_CHECK(handler.handle(gf_a, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK(handler.handle(gf_10, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(gf_a.reg(Gpr::kRax), 0ULL);
    HYPERION_CHECK_EQ(gf_10.reg(Gpr::kRax), 1ULL);

    // Positional pwrite64/pread64 leave file offset untouched at 0.
    GuestState pw = syscall_state(hyperion::sysno::kPwrite64, {fd_a, kBase + 0x500, 7, 4});
    HYPERION_CHECK(handler.handle(pw, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(pw.reg(Gpr::kRax), 7ULL);

    GuestState ls = syscall_state(hyperion::sysno::kLseek, {fd_b, 0, hyperion::seek::kCur});
    HYPERION_CHECK(handler.handle(ls, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(ls.reg(Gpr::kRax), 0ULL);

    GuestState pr = syscall_state(hyperion::sysno::kPread64, {fd_b, kBase + 0x700, 7, 4});
    HYPERION_CHECK(handler.handle(pr, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(pr.reg(Gpr::kRax), 7ULL);
    char check_buf[8] = {};
    HYPERION_CHECK(memory.read(kBase + 0x700, reinterpret_cast<std::uint8_t*>(check_buf), 7));
    HYPERION_CHECK_EQ(std::string(check_buf), std::string("Hello, "));
}

// --- linux-compat layer: sleeping -------------------------------------------
//
// nanosleep used to be a stub that returned 0 immediately, which turned every
// guest retry/backoff loop into a busy loop. These tests pin the behaviour that
// matters: the call really waits, and its argument validation matches Linux.

namespace {

// Writes a guest timespec at `addr`.
void write_timespec(FlatMemory& memory, std::uint64_t addr, std::int64_t sec, std::int64_t nsec) {
    std::array<std::uint8_t, 16> buf{};
    hyperion::syscall_detail::put_u64(buf.data(), 0, static_cast<std::uint64_t>(sec));
    hyperion::syscall_detail::put_u64(buf.data(), 8, static_cast<std::uint64_t>(nsec));
    HYPERION_CHECK(memory.write(addr, buf.data(), buf.size()));
}

} // namespace

HYPERION_TEST("syscall", "linux_compat_nanosleep_actually_sleeps") {
    GuestAddressSpace as;
    MinimalSyscallHandler handler({}, &as);
    handler.enable_linux_compat();
    FlatMemory memory(kBase, kMemSize);
    write_timespec(memory, kBase + 0x100, 0, 30 * 1000 * 1000); // 30 ms

    GuestState s = syscall_state(35 /* nanosleep */, {kBase + 0x100, kBase + 0x200});
    const auto start = std::chrono::steady_clock::now();
    const auto r = handler.handle(s, memory);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{0});
    // A scheduler can oversleep; it cannot under-sleep by 5 ms.
    HYPERION_CHECK(elapsed >= 25);
}

HYPERION_TEST("syscall", "linux_compat_nanosleep_rejects_bad_arguments") {
    GuestAddressSpace as;
    MinimalSyscallHandler handler({}, &as);
    handler.enable_linux_compat();
    FlatMemory memory(kBase, kMemSize);

    // tv_nsec must be < 1e9: Linux rejects anything else with EINVAL rather than
    // normalising it.
    write_timespec(memory, kBase + 0x100, 0, 1000000000);
    GuestState s = syscall_state(35, {kBase + 0x100, 0});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEinval));

    // Negative tv_sec.
    write_timespec(memory, kBase + 0x100, -1, 0);
    s = syscall_state(35, {kBase + 0x100, 0});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEinval));

    // A pointer outside the guest's memory is a fault, not a host crash.
    s = syscall_state(35, {0xDEAD0000ULL, 0});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));

    // The same for clock_nanosleep, whose request is argument 2.
    s = syscall_state(230, {0 /*CLOCK_REALTIME*/, 0, 0xDEAD0000ULL, 0});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));

    // TIMER_ABSTIME is the only flag clock_nanosleep accepts.
    write_timespec(memory, kBase + 0x100, 0, 0);
    s = syscall_state(230, {1 /*CLOCK_MONOTONIC*/, 0x40 /*not TIMER_ABSTIME*/, kBase + 0x100, 0});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEinval));
}

HYPERION_TEST("syscall", "linux_compat_clock_nanosleep_absolute_deadline_is_immediate") {
    GuestAddressSpace as;
    MinimalSyscallHandler handler({}, &as);
    handler.enable_linux_compat();
    FlatMemory memory(kBase, kMemSize);

    // A deadline in the past must return at once rather than sleeping until the
    // epoch rolls round again.
    write_timespec(memory, kBase + 0x100, 1, 0);
    GuestState s =
        syscall_state(230, {0 /*CLOCK_REALTIME*/, 1 /*TIMER_ABSTIME*/, kBase + 0x100, 0});
    const auto start = std::chrono::steady_clock::now();
    handler.handle(s, memory);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{0});
    HYPERION_CHECK(elapsed < 1000);
}

// --- linux-compat layer: terminal ioctl -------------------------------------
//
// Raw mode is the first thing a terminal application does, and it is a TCSETS
// ioctl. These tests drive the ioctl path over a real pseudo-terminal, because
// that is the only way to know the translated request numbers and the 36-byte
// kernel `struct termios` actually line up.

namespace {

// A pseudo-terminal pair, or nothing if the host cannot provide one.
struct PtyPair {
    int master = -1;
    int slave = -1;
    bool ok() const { return master >= 0 && slave >= 0; }
    ~PtyPair() {
        if (slave >= 0) {
            ::close(slave);
        }
        if (master >= 0) {
            ::close(master);
        }
    }
};

PtyPair open_pty() {
    PtyPair pty;
    pty.master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (pty.master < 0) {
        return pty;
    }
    if (::grantpt(pty.master) != 0 || ::unlockpt(pty.master) != 0) {
        return pty;
    }
    const char* name = ::ptsname(pty.master);
    if (name == nullptr) {
        return pty;
    }
    pty.slave = ::open(name, O_RDWR | O_NOCTTY);
    return pty;
}

// The guest's view of the kernel termios: 4 flag words, c_line, c_cc[19].
constexpr std::size_t kTermiosBytes = 36;
constexpr std::uint64_t kIoctlTcgets = 0x5401;
constexpr std::uint64_t kIoctlTcsets = 0x5402;
constexpr std::uint64_t kIoctlTcsetsw = 0x5403;
constexpr std::uint64_t kIoctlTcsetsf = 0x5404;
constexpr std::uint64_t kIoctlTiocswinsz = 0x5414;
constexpr std::uint64_t kIoctlFionread = 0x541B;

} // namespace

HYPERION_TEST("syscall", "linux_compat_ioctl_tcgets_then_tcsets_round_trips") {
    PtyPair pty = open_pty();
    if (!pty.ok()) {
        HYPERION_SKIP("host has no pseudo-terminal");
    }
    GuestAddressSpace as;
    MinimalSyscallHandler handler({}, &as);
    handler.enable_linux_compat();
    handler.adopt(3, ::fdopen(pty.slave, "r+"));
    pty.slave = -1; // now owned by the handler

    FlatMemory memory(kBase, kMemSize);

    // TCGETS: the guest asks for the current terminal attributes.
    GuestState s = syscall_state(16 /* ioctl */, {3, kIoctlTcgets, kBase + 0x100});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{0});
    std::array<std::uint8_t, kTermiosBytes> original{};
    HYPERION_CHECK(memory.read(kBase + 0x100, original.data(), original.size()));
    // A fresh pty has ECHO set in c_lflag (word 3). If this were zero the test
    // would pass whatever the handler did.
    std::uint32_t lflag = 0;
    std::memcpy(&lflag, original.data() + 12, 4);
    HYPERION_CHECK((lflag & 0x8U) != 0); // ECHO

    // TCSETS with ECHO and ICANON cleared: raw mode, the call a TUI makes.
    std::array<std::uint8_t, kTermiosBytes> raw = original;
    std::uint32_t iflag = 0;
    std::memcpy(&iflag, raw.data(), 4);
    std::memcpy(raw.data(), &iflag, 4);
    lflag &= ~0x8U; // ECHO
    lflag &= ~0x2U; // ICANON
    std::memcpy(raw.data() + 12, &lflag, 4);
    HYPERION_CHECK(memory.write(kBase + 0x200, raw.data(), raw.size()));

    s = syscall_state(16, {3, kIoctlTcsets, kBase + 0x200});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{0});

    // Read back through the handler: the change must have reached the host.
    std::array<std::uint8_t, kTermiosBytes> after{};
    HYPERION_CHECK(memory.write(kBase + 0x100, after.data(), after.size()));
    s = syscall_state(16, {3, kIoctlTcgets, kBase + 0x100});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{0});
    std::uint32_t lflag_after = 0;
    HYPERION_CHECK(memory.read(kBase + 0x100, after.data(), after.size()));
    std::memcpy(&lflag_after, after.data() + 12, 4);
    HYPERION_CHECK((lflag_after & 0x8U) == 0); // ECHO cleared
    HYPERION_CHECK((lflag_after & 0x2U) == 0); // ICANON cleared

    // TCSETSW and TCSETSF are the same request with a different wait point.
    s = syscall_state(16, {3, kIoctlTcsetsw, kBase + 0x100});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{0});
    s = syscall_state(16, {3, kIoctlTcsetsf, kBase + 0x100});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{0});
}

HYPERION_TEST("syscall", "linux_compat_ioctl_tiocswinsz_and_fionread") {
    PtyPair pty = open_pty();
    if (!pty.ok()) {
        HYPERION_SKIP("host has no pseudo-terminal");
    }
    GuestAddressSpace as;
    MinimalSyscallHandler handler({}, &as);
    handler.enable_linux_compat();
    handler.adopt(3, ::fdopen(pty.slave, "r+"));
    pty.slave = -1;
    FlatMemory memory(kBase, kMemSize);

    // TIOCSWINSZ: the guest sets the window size (a resize the TUI asked for).
    std::array<std::uint8_t, 8> ws{};
    hyperion::syscall_detail::put_u16(ws.data(), 0, 40);  // ws_row
    hyperion::syscall_detail::put_u16(ws.data(), 2, 100); // ws_col
    HYPERION_CHECK(memory.write(kBase + 0x100, ws.data(), ws.size()));
    GuestState s = syscall_state(16, {3, kIoctlTiocswinsz, kBase + 0x100});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{0});

    // Read it back with TIOCGWINSZ: the handler's existing case must see it.
    std::array<std::uint8_t, 8> back{};
    HYPERION_CHECK(memory.write(kBase + 0x200, back.data(), back.size()));
    s = syscall_state(16, {3, 0x5413 /*TIOCGWINSZ*/, kBase + 0x200});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{0});
    HYPERION_CHECK(memory.read(kBase + 0x200, back.data(), back.size()));
    HYPERION_CHECK_EQ(hyperion::syscall_detail::get_u16_le(back.data()), std::uint16_t{40});
    HYPERION_CHECK_EQ(hyperion::syscall_detail::get_u16_le(back.data() + 2), std::uint16_t{100});

    // FIONREAD: bytes waiting to be read. Measured on a pipe rather than through
    // the pty, because a pty's answer depends on the line discipline and on the
    // host kernel's buffer accounting - `ioctl(FIONREAD)` on this host's pty
    // reports 0 even while a read returns data. A pipe is exact, so the number can
    // be asserted instead of merely the return code.
    int pipe_fds[2] = {-1, -1};
    HYPERION_CHECK_EQ(::pipe(pipe_fds), 0);
    HYPERION_CHECK_EQ(::write(pipe_fds[1], "abc", 3), ssize_t{3});
    // adopt() takes ownership of the stream, which owns the descriptor.
    HYPERION_CHECK(handler.adopt(4, ::fdopen(pipe_fds[0], "r")));
    s = syscall_state(16, {4, kIoctlFionread, kBase + 0x300});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{0});
    std::array<std::uint8_t, 4> count{};
    HYPERION_CHECK(memory.read(kBase + 0x300, count.data(), count.size()));
    HYPERION_CHECK_EQ(hyperion::syscall_detail::get_u32_le(count.data()), std::uint32_t{3});
    HYPERION_CHECK_EQ(::close(pipe_fds[1]), 0);

    // On a terminal whose buffer the host reports as empty, the translating path
    // must still succeed and report what the host reported rather than inventing
    // a number.
    s = syscall_state(16, {3, kIoctlFionread, kBase + 0x300});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{0});
}

HYPERION_TEST("syscall", "linux_compat_ioctl_rejects_bad_pointers_and_unknown_requests") {
    GuestAddressSpace as;
    MinimalSyscallHandler handler({}, &as);
    handler.enable_linux_compat();
    FlatMemory memory(kBase, kMemSize);

    // A bad fd is EBADF; an unhandled request on a valid fd is ENOTTY, which is
    // what a real kernel answers - not ENOSYS, which would tell a caller the
    // translator is unfinished rather than the device does not support it.
    GuestState s = syscall_state(16, {9, kIoctlTcgets, kBase + 0x100});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEbadf));

    s = syscall_state(16, {1, 0xDEADBEEFULL, kBase + 0x100});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnotty));

    // A TCSETS whose buffer is outside the guest's memory is EFAULT, not a crash.
    s = syscall_state(16, {1, kIoctlTcsets, 0xDEAD0000ULL});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));
}
