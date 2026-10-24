// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Internal helpers shared between the core syscall handler (src/core/syscall.cpp)
// and the compatibility-adapter layer (src/core/syscall_adapters.cpp).
//
// This header is *internal*: it is installed for tests and sibling translation
// units inside the project, and its declarations may change with any syscall
// work. It exists so the adapter layer reuses the one implementation of guest
// memory/fd/path handling rather than growing a second, subtly different one.
//
// Every function here takes guest memory by contract (`Memory&`) and validates
// it; nothing dereferences a raw guest pointer.
#ifndef HYPERION_SYSCALL_DETAIL_HPP
#define HYPERION_SYSCALL_DETAIL_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/syscall.hpp"

namespace hyperion::syscall_detail {

// The x86-64 syscall argument registers, in order. r10 (not rcx) is the fourth
// argument: the `syscall` instruction clobbers rcx with the return address.
inline constexpr Gpr kArgRegs[6] = {Gpr::kRdi, Gpr::kRsi, Gpr::kRdx, Gpr::kR10, Gpr::kR8, Gpr::kR9};

// AT_FDCWD as the guest's 32-bit signed value.
inline constexpr std::int32_t kGuestAtFdcwd = -100;

std::uint64_t arg(const GuestState& s, int index);

// Truncate to the kernel's 32-bit `int` view of an argument. On Linux x86-64 a
// C `int` parameter ignores the upper 32 bits of the register, so
// `mov edi, -100` yields 0x00000000ffffff9c and must still compare equal to -100.
std::int32_t arg_i32(const GuestState& s, int index);

const GuestDescriptor* lookup_fd(const std::vector<GuestDescriptor>& files, std::int32_t fd);
const OpenFileDescription* lookup_ofd(const std::vector<GuestDescriptor>& files, std::int32_t fd);

// Host descriptor backing a guest open file description, or -1 when the
// description has no underlying host fd (an adopted stdio stream without one).
int underlying_host_fd(const OpenFileDescription& ofd);

// Allocate the lowest free guest descriptor >= min_fd, as the kernel does.
std::int64_t alloc_fd(std::vector<GuestDescriptor>& files,
                      std::shared_ptr<OpenFileDescription> desc, bool cloexec,
                      std::size_t min_fd = 3);

// Copy `count` bytes from a guest buffer into an already-resolved descriptor,
// chunked so a huge count cannot force one huge host allocation.
std::int64_t read_segment(const OpenFileDescription& ofd, Memory& memory, std::uint64_t buf,
                          std::uint64_t count, bool& short_transfer);

std::int64_t write_segment(const OpenFileDescription& ofd, Memory& memory,
                           const MinimalSyscallHandler::OutputSink& sink, std::uint64_t buf,
                           std::uint64_t count, bool& short_transfer);

// Read a NUL-terminated guest string with a PATH_MAX bound.
std::int64_t read_guest_cstr(Memory& memory, std::uint64_t address, std::string& out);

// Prefix an absolute guest path with the configured sysroot when the prefixed
// path exists.
std::string resolve_guest_path(const std::string& path, const std::string& sysroot);

// Map a guest dirfd to a host dirfd. Returns false when the mapping is not
// supported at all (the caller turns that into -ENOSYS); `out_host_dirfd` is set
// to -1 when the guest descriptor is closed (the caller turns that into -EBADF).
bool resolve_host_dirfd(const std::vector<GuestDescriptor>& files, std::int32_t dirfd, bool compat,
                        int& out_host_dirfd);

struct OpenFlags {
    int host = 0;
    bool implemented = true;
};

// Translate guest O_* flags to host O_* flags.  O_DIRECTORY and other values
// differ between x86-64 and AArch64 and are therefore mapped explicitly.
OpenFlags translate_open_flags(std::uint64_t flags, bool compat = false);

void put_u64(std::uint8_t* buf, std::size_t off, std::uint64_t value);
void put_u32(std::uint8_t* buf, std::size_t off, std::uint32_t value);
void put_u16(std::uint8_t* buf, std::size_t off, std::uint16_t value);
// Little-endian readers. Each one reads exactly the number of bytes its type is
// wide: reading a `short` with get_u64_le() overran an 8-byte packed buffer, which
// only AddressSanitizer noticed, because the overrun landed in the same stack
// frame.
std::uint64_t get_u64_le(const std::uint8_t* p);
std::uint32_t get_u32_le(const std::uint8_t* p);
std::uint16_t get_u16_le(const std::uint8_t* p);

// Serialise a host `struct stat` into the *guest's* 144-byte x86-64 layout.
std::int64_t write_guest_stat(Memory& memory, std::uint64_t buf, const struct stat& st);

} // namespace hyperion::syscall_detail

#endif // HYPERION_SYSCALL_DETAIL_HPP
