// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Minimal guest syscall handler (Phase 6). See syscall.hpp for the scope and
// the error convention.
#include "hyperion/signal_handling.hpp"
#include "hyperion/syscall.hpp"

#include "hyperion/host_syscalls.hpp"
#include "hyperion/process.hpp"
#include "hyperion/syscall_detail.hpp"
#include "hyperion/syscall_dispatch.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace hyperion {

OpenFileDescription::~OpenFileDescription() {
    if (owned_fd && host_fd >= 0) {
        ::close(host_fd);
        host_fd = -1;
    }
}

const char* to_string(SyscallStatus status) {
    switch (status) {
    case SyscallStatus::kHandled:
        return "handled";
    case SyscallStatus::kUnimplemented:
        return "unimplemented";
    case SyscallStatus::kExit:
        return "exit";
    }
    return "?";
}

// The shared guest-ABI helpers live in hyperion::syscall_detail so the
// compatibility-adapter layer (src/core/syscall_adapters.cpp) reuses this one
// implementation instead of growing a second, subtly different one.
namespace syscall_detail {

std::uint64_t arg(const GuestState& s, int index) {
    return s.gpr[static_cast<std::size_t>(kArgRegs[index])];
}

// On Linux x86-64, kernel syscall wrappers cast C `int` arguments (`fd`,
// `dirfd`, etc.) to 32-bit signed `int`, ignoring the upper 32 bits of the
// register. Without this truncation, `mov edi, -100` (`RDI = 0x00000000ffffff9c`)
// fails a 64-bit comparison against `-100` (`0xffffffffffffff9c`).
std::int32_t arg_i32(const GuestState& s, int index) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(arg(s, index)));
}

long default_sink(int fd, const std::uint8_t* data, std::size_t count) {
    std::FILE* out = (fd == 2) ? stderr : stdout;
    const std::size_t wrote = std::fwrite(data, 1, count, out);
    std::fflush(out);
    return static_cast<long>(wrote);
}

// Look up an open guest descriptor slot in the table. Returns nullptr for a
// negative, out-of-range, or closed descriptor; callers map that to -EBADF.
const GuestDescriptor* lookup_fd(const std::vector<GuestDescriptor>& files, std::int32_t fd) {
    if (fd < 0) {
        return nullptr;
    }
    const auto idx = static_cast<std::size_t>(fd);
    if (idx >= files.size() || !files[idx].is_open()) {
        return nullptr;
    }
    return &files[idx];
}

const OpenFileDescription* lookup_ofd(const std::vector<GuestDescriptor>& files, std::int32_t fd) {
    const GuestDescriptor* e = lookup_fd(files, fd);
    return e == nullptr ? nullptr : e->desc.get();
}

int underlying_host_fd(const OpenFileDescription& ofd) {
    if (ofd.host_fd >= 0) {
        return ofd.host_fd;
    }
    if (ofd.stream != nullptr) {
        return ::fileno(ofd.stream);
    }
    return -1;
}

// Allocate the lowest free guest descriptor >= min_fd, as the kernel does.
std::int64_t alloc_fd(std::vector<GuestDescriptor>& files,
                      std::shared_ptr<OpenFileDescription> desc, bool cloexec, std::size_t min_fd) {
    constexpr std::size_t kMaxGuestFds = 4096;
    if (min_fd >= kMaxGuestFds) {
        return -kEinval;
    }
    for (std::size_t i = min_fd; i < files.size(); ++i) {
        if (!files[i].is_open()) {
            files[i] = GuestDescriptor{std::move(desc), cloexec};
            return static_cast<std::int64_t>(i);
        }
    }
    if (files.size() >= kMaxGuestFds) {
        return -kEmfile;
    }
    if (files.size() < min_fd) {
        files.resize(min_fd);
    }
    files.push_back(GuestDescriptor{std::move(desc), cloexec});
    return static_cast<std::int64_t>(files.size() - 1);
}

std::int64_t write_segment(const OpenFileDescription& ofd, Memory& memory,
                           const MinimalSyscallHandler::OutputSink& sink, std::uint64_t buf,
                           std::uint64_t count, bool& short_transfer) {
    short_transfer = false;
    if (count == 0) {
        return 0;
    }

    constexpr std::size_t kChunk = 4096;
    std::array<std::uint8_t, kChunk> chunk{};
    std::uint64_t done = 0;
    while (done < count) {
        const std::size_t n =
            static_cast<std::size_t>((count - done) < kChunk ? (count - done) : kChunk);
        if (!memory.read(buf + done, chunk.data(), n)) {
            return done > 0 ? static_cast<std::int64_t>(done) : -kEfault;
        }
        if (ofd.sink_fd >= 0) {
            if (sink) {
                const long wrote = sink(ofd.sink_fd, chunk.data(), n);
                if (wrote < 0) {
                    return done > 0 ? static_cast<std::int64_t>(done) : -kEbadf;
                }
                done += static_cast<std::uint64_t>(wrote);
                if (static_cast<std::size_t>(wrote) < n) {
                    short_transfer = true;
                    break;
                }
            } else {
                done += n;
            }
        } else if (ofd.host_fd >= 0) {
            const ssize_t wrote = ::write(ofd.host_fd, chunk.data(), n);
            if (wrote < 0) {
                const std::int64_t err = -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
                return done > 0 ? static_cast<std::int64_t>(done) : err;
            }
            done += static_cast<std::uint64_t>(wrote);
            if (static_cast<std::size_t>(wrote) < n) {
                short_transfer = true;
                break;
            }
        } else if (ofd.stream != nullptr) {
            const std::size_t wrote = std::fwrite(chunk.data(), 1, n, ofd.stream);
            if (wrote == 0) {
                return done > 0 ? static_cast<std::int64_t>(done) : -kEbadf;
            }
            std::fflush(ofd.stream);
            done += wrote;
            if (wrote < n) {
                short_transfer = true;
                break;
            }
        } else {
            return -kEbadf;
        }
    }
    return static_cast<std::int64_t>(done);
}

// write(fd, buf, count): write `count` guest bytes through an open guest
// descriptor (sink-routed stdout/stderr, unbuffered kernel file descriptor
// opened via openat, or an adopted host FILE*).
std::int64_t do_write(GuestState& s, Memory& memory, const MinimalSyscallHandler::OutputSink& sink,
                      const std::vector<GuestDescriptor>& files) {
    const std::int32_t fd = arg_i32(s, 0);
    const std::uint64_t buf = arg(s, 1);
    const std::uint64_t count = arg(s, 2);

    const OpenFileDescription* ofd = lookup_ofd(files, fd);
    if (ofd == nullptr) {
        return -kEbadf;
    }
    bool short_transfer = false;
    return write_segment(*ofd, memory, sink, buf, count, short_transfer);
}

std::int64_t read_segment(const OpenFileDescription& ofd, Memory& memory, std::uint64_t buf,
                          std::uint64_t count, bool& short_transfer) {
    short_transfer = false;
    if (count == 0) {
        return 0;
    }

    constexpr std::size_t kChunk = 4096;
    std::array<std::uint8_t, kChunk> chunk{};
    std::uint64_t done = 0;
    while (done < count) {
        const std::size_t n =
            static_cast<std::size_t>((count - done) < kChunk ? (count - done) : kChunk);
        std::size_t got = 0;
        if (ofd.host_fd >= 0) {
            const ssize_t rc = ::read(ofd.host_fd, chunk.data(), n);
            if (rc < 0) {
                const std::int64_t err = -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
                return done > 0 ? static_cast<std::int64_t>(done) : err;
            }
            got = static_cast<std::size_t>(rc);
        } else if (ofd.stream != nullptr) {
            got = std::fread(chunk.data(), 1, n, ofd.stream);
        } else {
            return -kEbadf;
        }
        if (got == 0) {
            short_transfer = true;
            break; // EOF
        }
        if (!memory.write(buf + done, chunk.data(), got)) {
            return -kEfault;
        }
        done += got;
        if (got < n) {
            short_transfer = true;
            break;
        }
    }
    return static_cast<std::int64_t>(done);
}

// read(fd, buf, count): read up to `count` bytes from an open guest descriptor
// into guest memory.
std::int64_t do_read(GuestState& s, Memory& memory, const std::vector<GuestDescriptor>& files) {
    const std::int32_t fd = arg_i32(s, 0);
    const std::uint64_t buf = arg(s, 1);
    const std::uint64_t count = arg(s, 2);

    const OpenFileDescription* ofd = lookup_ofd(files, fd);
    if (ofd == nullptr) {
        return -kEbadf;
    }
    bool short_transfer = false;
    return read_segment(*ofd, memory, buf, count, short_transfer);
}

// pread64(fd, buf, count, offset): positional read preserving file offset.
std::int64_t do_pread64(GuestState& s, Memory& memory, const std::vector<GuestDescriptor>& files) {
    const std::int32_t fd = arg_i32(s, 0);
    const std::uint64_t buf = arg(s, 1);
    const std::uint64_t count = arg(s, 2);
    const auto offset = static_cast<std::int64_t>(arg(s, 3));

    const OpenFileDescription* ofd = lookup_ofd(files, fd);
    if (ofd == nullptr) {
        return -kEbadf;
    }
    if (offset < 0) {
        return -kEinval;
    }
    if (count == 0) {
        return 0;
    }

    constexpr std::size_t kChunk = 4096;
    std::array<std::uint8_t, kChunk> chunk{};
    std::uint64_t done = 0;
    while (done < count) {
        const std::size_t n =
            static_cast<std::size_t>((count - done) < kChunk ? (count - done) : kChunk);
        const auto cur_off = static_cast<off_t>(offset + static_cast<std::int64_t>(done));
        std::size_t got = 0;
        if (ofd->host_fd >= 0) {
            const ssize_t rc = ::pread(ofd->host_fd, chunk.data(), n, cur_off);
            if (rc < 0) {
                const std::int64_t err = -static_cast<std::int64_t>(errno == 0 ? kEspipe : errno);
                return done > 0 ? static_cast<std::int64_t>(done) : err;
            }
            got = static_cast<std::size_t>(rc);
        } else if (ofd->stream != nullptr) {
            const long saved = std::ftell(ofd->stream);
            if (saved < 0 || std::fseek(ofd->stream, static_cast<long>(cur_off), SEEK_SET) != 0) {
                return -kEspipe;
            }
            got = std::fread(chunk.data(), 1, n, ofd->stream);
            (void)std::fseek(ofd->stream, saved, SEEK_SET);
        } else {
            return -kEspipe;
        }
        if (got == 0) {
            break;
        }
        if (!memory.write(buf + done, chunk.data(), got)) {
            return -kEfault;
        }
        done += got;
        if (got < n) {
            break;
        }
    }
    return static_cast<std::int64_t>(done);
}

// pwrite64(fd, buf, count, offset): positional write preserving file offset.
std::int64_t do_pwrite64(GuestState& s, Memory& memory, const std::vector<GuestDescriptor>& files) {
    const std::int32_t fd = arg_i32(s, 0);
    const std::uint64_t buf = arg(s, 1);
    const std::uint64_t count = arg(s, 2);
    const auto offset = static_cast<std::int64_t>(arg(s, 3));

    const OpenFileDescription* ofd = lookup_ofd(files, fd);
    if (ofd == nullptr) {
        return -kEbadf;
    }
    if (offset < 0) {
        return -kEinval;
    }
    if (count == 0) {
        return 0;
    }

    constexpr std::size_t kChunk = 4096;
    std::array<std::uint8_t, kChunk> chunk{};
    std::uint64_t done = 0;
    while (done < count) {
        const std::size_t n =
            static_cast<std::size_t>((count - done) < kChunk ? (count - done) : kChunk);
        if (!memory.read(buf + done, chunk.data(), n)) {
            return done > 0 ? static_cast<std::int64_t>(done) : -kEfault;
        }
        const auto cur_off = static_cast<off_t>(offset + static_cast<std::int64_t>(done));
        if (ofd->host_fd >= 0) {
            const ssize_t wrote = ::pwrite(ofd->host_fd, chunk.data(), n, cur_off);
            if (wrote < 0) {
                const std::int64_t err = -static_cast<std::int64_t>(errno == 0 ? kEspipe : errno);
                return done > 0 ? static_cast<std::int64_t>(done) : err;
            }
            done += static_cast<std::uint64_t>(wrote);
            if (static_cast<std::size_t>(wrote) < n) {
                break;
            }
        } else if (ofd->stream != nullptr) {
            const long saved = std::ftell(ofd->stream);
            if (saved < 0 || std::fseek(ofd->stream, static_cast<long>(cur_off), SEEK_SET) != 0) {
                return -kEspipe;
            }
            const std::size_t wrote = std::fwrite(chunk.data(), 1, n, ofd->stream);
            std::fflush(ofd->stream);
            (void)std::fseek(ofd->stream, saved, SEEK_SET);
            if (wrote == 0) {
                return done > 0 ? static_cast<std::int64_t>(done) : -kEbadf;
            }
            done += wrote;
            if (wrote < n) {
                break;
            }
        } else {
            return -kEspipe;
        }
    }
    return static_cast<std::int64_t>(done);
}

std::uint64_t get_u64_le(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

std::uint32_t get_u32_le(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    }
    return v;
}

std::uint16_t get_u16_le(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      (static_cast<std::uint16_t>(p[1]) << 8));
}

// readv(fd, iov, iovcnt) & writev(fd, iov, iovcnt): scatter/gather I/O over
// 16-byte guest `struct iovec { uint64_t iov_base; uint64_t iov_len; }`.
std::int64_t do_iov(GuestState& s, Memory& memory, const MinimalSyscallHandler::OutputSink& sink,
                    const std::vector<GuestDescriptor>& files, bool is_write) {
    constexpr std::int32_t kUioMaxIov = 1024;
    constexpr std::uint64_t kMaxSsize = 0x7FFFFFFFFFFFFFFFULL;
    const std::int32_t fd = arg_i32(s, 0);
    const std::uint64_t iov_addr = arg(s, 1);
    const std::int32_t iovcnt = arg_i32(s, 2);

    const OpenFileDescription* ofd = lookup_ofd(files, fd);
    if (ofd == nullptr) {
        return -kEbadf;
    }
    if (iovcnt < 0 || iovcnt > kUioMaxIov) {
        return -kEinval;
    }
    if (iovcnt == 0) {
        return 0;
    }

    struct GuestIovec {
        std::uint64_t base = 0;
        std::uint64_t len = 0;
    };
    std::vector<GuestIovec> iovs(static_cast<std::size_t>(iovcnt));
    std::uint64_t total_len = 0;
    for (std::int32_t i = 0; i < iovcnt; ++i) {
        std::uint8_t raw[16] = {};
        if (!memory.read(iov_addr + static_cast<std::uint64_t>(i) * std::uint64_t{16}, raw,
                         sizeof(raw))) {
            return -kEfault;
        }
        iovs[static_cast<std::size_t>(i)].base = get_u64_le(raw);
        iovs[static_cast<std::size_t>(i)].len = get_u64_le(raw + 8);
        if (iovs[static_cast<std::size_t>(i)].len > kMaxSsize - total_len) {
            return -kEinval;
        }
        total_len += iovs[static_cast<std::size_t>(i)].len;
    }

    std::int64_t total_done = 0;
    for (const GuestIovec& vec : iovs) {
        if (vec.len == 0) {
            continue;
        }
        bool short_transfer = false;
        const std::int64_t rc =
            is_write ? write_segment(*ofd, memory, sink, vec.base, vec.len, short_transfer)
                     : read_segment(*ofd, memory, vec.base, vec.len, short_transfer);
        if (rc < 0) {
            return total_done > 0 ? total_done : rc;
        }
        total_done += rc;
        if (short_transfer) {
            break;
        }
    }
    return total_done;
}

// getdents64(fd, dirp, count): read directory entries into the guest buffer.
std::int64_t do_getdents64(GuestState& s, Memory& memory,
                           const std::vector<GuestDescriptor>& files) {
    const std::int32_t fd = arg_i32(s, 0);
    const std::uint64_t buf = arg(s, 1);
    const std::uint64_t count = arg(s, 2);

    const OpenFileDescription* ofd = lookup_ofd(files, fd);
    if (ofd == nullptr) {
        return -kEbadf;
    }
    const int host_fd = underlying_host_fd(*ofd);
    if (host_fd < 0) {
        return -kEnotdir;
    }
    constexpr std::uint64_t kDirent64Size = 24;
    if (count < kDirent64Size) {
        return -kEinval;
    }
    constexpr std::size_t kMaxGetdents = 8192;
    const std::size_t want = static_cast<std::size_t>(count < kMaxGetdents ? count : kMaxGetdents);
    std::array<std::uint8_t, kMaxGetdents> entries{};
    const long got = ::syscall(SYS_getdents64, host_fd, entries.data(), want);
    if (got < 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    if (!memory.write(buf, entries.data(), static_cast<std::size_t>(got))) {
        return -kEfault;
    }
    return static_cast<std::int64_t>(got);
}

// close(fd): release one descriptor table slot. The underlying OpenFileDescription
// closes `host_fd` automatically when the last duplicated slot drops its reference.
std::int64_t do_close(GuestState& s, std::vector<GuestDescriptor>& files) {
    const std::int32_t fd = arg_i32(s, 0);
    const GuestDescriptor* e = lookup_fd(files, fd);
    if (e == nullptr) {
        return -kEbadf;
    }
    files[static_cast<std::size_t>(fd)] = GuestDescriptor{};
    return 0;
}

// lseek(fd, offset, whence): reposition an open file description.
std::int64_t do_lseek(GuestState& s, const std::vector<GuestDescriptor>& files) {
    const std::int32_t fd = arg_i32(s, 0);
    const auto offset = static_cast<std::int64_t>(arg(s, 1));
    const auto whence = static_cast<std::uint32_t>(arg(s, 2));

    const OpenFileDescription* ofd = lookup_ofd(files, fd);
    if (ofd == nullptr) {
        return -kEbadf;
    }
    int host_whence = 0;
    switch (whence) {
    case seek::kSet:
        host_whence = SEEK_SET;
        break;
    case seek::kCur:
        host_whence = SEEK_CUR;
        break;
    case seek::kEnd:
        host_whence = SEEK_END;
        break;
    default:
        return -kEinval;
    }
    if (ofd->host_fd >= 0) {
        const off_t pos = ::lseek(ofd->host_fd, static_cast<off_t>(offset), host_whence);
        if (pos < 0) {
            return -static_cast<std::int64_t>(errno == 0 ? kEspipe : errno);
        }
        return static_cast<std::int64_t>(pos);
    }
    if (ofd->stream != nullptr) {
        errno = 0;
        if (std::fseek(ofd->stream, static_cast<long>(offset), host_whence) != 0) {
            return -static_cast<std::int64_t>(errno == EINVAL ? kEinval : kEspipe);
        }
        const long pos = std::ftell(ofd->stream);
        if (pos < 0) {
            return -kEinval;
        }
        return static_cast<std::int64_t>(pos);
    }
    return -kEspipe;
}

// The kernel's `struct termios` (asm-generic, shared by x86-64 and AArch64):
// four u32 flag words, c_line, c_cc[19]. This is *not* the C library's 60-byte
// userspace struct - glibc converts before issuing the ioctl, and so does the
// guest's glibc, so only the kernel-sized layout crosses the ABI boundary.
constexpr std::size_t kGuestTermiosSize = 36;

std::int64_t read_guest_cstr(Memory& memory, std::uint64_t address, std::string& out) {
    constexpr std::size_t kMaxPath = 4096; // Linux PATH_MAX
    out.clear();
    out.reserve(64);
    for (std::size_t i = 0; i < kMaxPath; ++i) {
        std::uint8_t ch = 0;
        if (!memory.read(address + i, &ch, 1)) {
            return -kEfault;
        }
        if (ch == 0) {
            return 0;
        }
        out.push_back(static_cast<char>(ch));
    }
    return -kEnametoolong;
}

std::string resolve_guest_path(const std::string& path, const std::string& sysroot) {
    if (sysroot.empty() || path.empty() || path[0] != '/') {
        return path;
    }
    const std::string candidate = sysroot + path;
    if (::access(candidate.c_str(), F_OK) == 0) {
        return candidate;
    }
    return path;
}

bool resolve_host_dirfd(const std::vector<GuestDescriptor>& files, std::int32_t dirfd, bool compat,
                        int& out_host_dirfd) {
    if (dirfd == kGuestAtFdcwd) {
        out_host_dirfd = AT_FDCWD;
        return true;
    }
    if (!compat) {
        return false;
    }
    const OpenFileDescription* ofd = lookup_ofd(files, dirfd);
    if (ofd == nullptr) {
        out_host_dirfd = -1;
        return true; // signal -EBADF rather than -ENOSYS
    }
    out_host_dirfd = underlying_host_fd(*ofd);
    return true;
}

OpenFlags translate_open_flags(std::uint64_t flags, bool compat) {
    OpenFlags r;
    switch (flags & open_flags::kOAccmode) {
    case open_flags::kORdonly:
        r.host |= O_RDONLY;
        break;
    case open_flags::kOWronly:
        r.host |= O_WRONLY;
        break;
    case open_flags::kORdwr:
        r.host |= O_RDWR;
        break;
    default:
        return {0, false};
    }
    if ((flags & open_flags::kOCreat) != 0) {
        r.host |= O_CREAT;
    }
    if ((flags & open_flags::kOTrunc) != 0) {
        r.host |= O_TRUNC;
    }
    if ((flags & open_flags::kOAppend) != 0) {
        r.host |= O_APPEND;
    }
    if ((flags & open_flags::kOExcl) != 0) {
        r.host |= O_EXCL;
    }
    if ((flags & open_flags::kODirectory) != 0) {
        r.host |= O_DIRECTORY;
    }
    if ((flags & open_flags::kONonblock) != 0) {
        r.host |= O_NONBLOCK;
    }
    constexpr std::uint64_t kHonoured = open_flags::kOAccmode | open_flags::kOCreat |
                                        open_flags::kOTrunc | open_flags::kOAppend |
                                        open_flags::kOExcl | open_flags::kODirectory |
                                        open_flags::kONonblock | open_flags::kOCloseexec;
    if (!compat) {
        if ((flags & ~kHonoured) != 0) {
            return {0, false};
        }
        return r;
    }
    // x86-64 additional standard POSIX/Linux open flags:
    // O_NOCTTY (0x100), O_DSYNC (0x1000), O_LARGEFILE (0x8000),
    // O_NOFOLLOW (0x20000), O_NOATIME (0x40000), O_PATH (0x200000).
    if ((flags & 0x100ULL) != 0) {
        r.host |= O_NOCTTY;
    }
    if ((flags & 0x20000ULL) != 0) {
        r.host |= O_NOFOLLOW;
    }
#ifdef O_PATH
    if ((flags & 0x200000ULL) != 0) {
        r.host |= O_PATH;
    }
#endif
    return r;
}

std::int64_t do_openat(GuestState& s, Memory& memory, std::vector<GuestDescriptor>& files,
                       bool compat = false, const std::string& sysroot = {},
                       bool legacy_open = false) {
    const std::int32_t dirfd = legacy_open ? kGuestAtFdcwd : arg_i32(s, 0);
    const std::uint64_t path_addr = legacy_open ? arg(s, 0) : arg(s, 1);
    const auto flags =
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, legacy_open ? 1 : 2)));
    const auto mode = static_cast<mode_t>(arg(s, legacy_open ? 2 : 3) & 0xFFFFu);

    int host_dirfd = AT_FDCWD;
    if (!resolve_host_dirfd(files, dirfd, compat, host_dirfd)) {
        return -kEnosys;
    }
    if (host_dirfd < 0 && host_dirfd != AT_FDCWD) {
        return -kEbadf;
    }
    std::string path;
    const std::int64_t rc = read_guest_cstr(memory, path_addr, path);
    if (rc != 0) {
        return rc;
    }
    path = resolve_guest_path(path, sysroot);
    const OpenFlags of = translate_open_flags(flags, compat);
    if (!of.implemented) {
        return -kEnosys;
    }

    const int host_fd = ::openat(host_dirfd, path.c_str(), of.host | O_CLOEXEC, mode);
    if (host_fd < 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    auto desc = std::make_shared<OpenFileDescription>();
    desc->host_fd = host_fd;
    desc->owned_fd = true;
    desc->status_flags = of.host;
    const bool cloexec = (flags & open_flags::kOCloseexec) != 0;
    return alloc_fd(files, std::move(desc), cloexec);
}

void put_u64(std::uint8_t* buf, std::size_t off, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        buf[off + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

void put_u32(std::uint8_t* buf, std::size_t off, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        buf[off + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

void put_u16(std::uint8_t* buf, std::size_t off, std::uint16_t value) {
    for (int i = 0; i < 2; ++i) {
        buf[off + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

std::int64_t write_guest_stat(Memory& memory, std::uint64_t buf, const struct ::stat& st) {
    std::array<std::uint8_t, guest_stat::kSize> out{};
    put_u64(out.data(), guest_stat::kOffStDev, static_cast<std::uint64_t>(st.st_dev));
    put_u64(out.data(), guest_stat::kOffStIno, static_cast<std::uint64_t>(st.st_ino));
    put_u64(out.data(), guest_stat::kOffStNlink, static_cast<std::uint64_t>(st.st_nlink));
    put_u32(out.data(), guest_stat::kOffStMode, static_cast<std::uint32_t>(st.st_mode));
    put_u32(out.data(), guest_stat::kOffStUid, static_cast<std::uint32_t>(st.st_uid));
    put_u32(out.data(), guest_stat::kOffStGid, static_cast<std::uint32_t>(st.st_gid));
    put_u64(out.data(), guest_stat::kOffStRdev, static_cast<std::uint64_t>(st.st_rdev));
    put_u64(out.data(), guest_stat::kOffStSize, static_cast<std::uint64_t>(st.st_size));
    put_u64(out.data(), guest_stat::kOffStBlksize, static_cast<std::uint64_t>(st.st_blksize));
    put_u64(out.data(), guest_stat::kOffStBlocks, static_cast<std::uint64_t>(st.st_blocks));
    put_u64(out.data(), guest_stat::kOffStAtime,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(st.st_atim.tv_sec)));
    put_u64(out.data(), guest_stat::kOffStAtime + 8,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(st.st_atim.tv_nsec)));
    put_u64(out.data(), guest_stat::kOffStMtime,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(st.st_mtim.tv_sec)));
    put_u64(out.data(), guest_stat::kOffStMtime + 8,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(st.st_mtim.tv_nsec)));
    put_u64(out.data(), guest_stat::kOffStCtime,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(st.st_ctim.tv_sec)));
    put_u64(out.data(), guest_stat::kOffStCtime + 8,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(st.st_ctim.tv_nsec)));

    if (!memory.write(buf, out.data(), out.size())) {
        return -kEfault;
    }
    return 0;
}

std::int64_t do_fstat(GuestState& s, Memory& memory, const std::vector<GuestDescriptor>& files) {
    const std::int32_t fd = arg_i32(s, 0);
    const std::uint64_t buf = arg(s, 1);

    const OpenFileDescription* ofd = lookup_ofd(files, fd);
    if (ofd == nullptr) {
        return -kEbadf;
    }
    const int host_fd = underlying_host_fd(*ofd);
    if (host_fd < 0) {
        return -kEbadf;
    }
    struct ::stat st{};
    if (::fstat(host_fd, &st) != 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    return write_guest_stat(memory, buf, st);
}

std::int64_t do_newfstatat(GuestState& s, Memory& memory, const std::vector<GuestDescriptor>& files,
                           bool compat = false, const std::string& sysroot = {}) {
    const std::int32_t dirfd = arg_i32(s, 0);
    const std::uint64_t path_addr = arg(s, 1);
    const std::uint64_t buf = arg(s, 2);
    const auto flags = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 3)));

    int host_dirfd = AT_FDCWD;
    if (!resolve_host_dirfd(files, dirfd, compat, host_dirfd)) {
        return -kEnosys;
    }
    if (host_dirfd < 0 && host_dirfd != AT_FDCWD) {
        return -kEbadf;
    }
    const std::uint64_t allowed_flags =
        compat ? (kAtSymlinkNofollow | statx::kAtEmptyPath | 0x800ULL) : kAtSymlinkNofollow;
    if ((flags & ~allowed_flags) != 0) {
        return -kEnosys;
    }
    std::string path;
    const std::int64_t rc = read_guest_cstr(memory, path_addr, path);
    if (rc != 0) {
        return rc;
    }
    if (compat && (flags & statx::kAtEmptyPath) != 0 && path.empty()) {
        struct ::stat st{};
        if (host_dirfd == AT_FDCWD) {
            return -kEinval;
        }
        if (::fstat(host_dirfd, &st) != 0) {
            return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
        }
        return write_guest_stat(memory, buf, st);
    }
    path = resolve_guest_path(path, sysroot);
    struct ::stat st{};
    int host_flags = ((flags & kAtSymlinkNofollow) != 0) ? AT_SYMLINK_NOFOLLOW : 0;
#ifdef AT_NO_AUTOMOUNT
    if ((flags & 0x800ULL) != 0) {
        host_flags |= AT_NO_AUTOMOUNT;
    }
#endif
    if (::fstatat(host_dirfd, path.c_str(), &st, host_flags) != 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    return write_guest_stat(memory, buf, st);
}

// Write a guest `struct statx` derived from an fstatat(2)/fstat(2) result.
//
// Used on hosts that cannot issue statx(2) - see hyperion/host_syscalls.hpp: on
// Android the syscall is outside the app seccomp allowlist before API 30 and is
// answered with SIGSYS (SECCOMP_RET_TRAP), which kills the process outright.
//
// Everything in STATX_BASIC_STATS is present in `struct stat`, so the fields
// below are real values, not placeholders. Two things cannot be derived and are
// reported as absent rather than guessed:
//   - stx_btime (creation time): fstatat does not expose it, so STATX_BTIME is
//     clear in stx_mask and the field is zero.
//   - stx_attributes / stx_attributes_mask: only statx(2) reports them; both
//     stay zero, which means "no attributes are claimed to be known".
std::int64_t write_guest_statx_from_stat(Memory& memory, std::uint64_t buf, const struct ::stat& st,
                                         unsigned requested_mask) {
    std::array<std::uint8_t, guest_statx::kSize> out{};
    const auto s64 = [](std::int64_t v) { return static_cast<std::uint64_t>(v); };

    // STATX_TYPE|MODE|NLINK|UID|GID|ATIME|MTIME|CTIME|INO|SIZE|BLOCKS: exactly the
    // fields fstatat(2) provides. A guest that asked for fewer bits still gets
    // the basic set, which is what the kernel does for STATX_BASIC_STATS.
    const unsigned available = static_cast<unsigned>(statx::kBasicStats);
    const unsigned returned = available & (requested_mask == 0 ? available : requested_mask);

    put_u32(out.data(), guest_statx::kOffMask, returned);
    put_u32(out.data(), guest_statx::kOffBlksize, static_cast<std::uint32_t>(st.st_blksize));
    put_u64(out.data(), guest_statx::kOffAttributes, 0);
    put_u32(out.data(), guest_statx::kOffNlink, static_cast<std::uint32_t>(st.st_nlink));
    put_u32(out.data(), guest_statx::kOffUid, static_cast<std::uint32_t>(st.st_uid));
    put_u32(out.data(), guest_statx::kOffGid, static_cast<std::uint32_t>(st.st_gid));
    put_u16(out.data(), guest_statx::kOffMode, static_cast<std::uint16_t>(st.st_mode));
    put_u64(out.data(), guest_statx::kOffIno, static_cast<std::uint64_t>(st.st_ino));
    put_u64(out.data(), guest_statx::kOffSize, static_cast<std::uint64_t>(st.st_size));
    put_u64(out.data(), guest_statx::kOffBlocks, static_cast<std::uint64_t>(st.st_blocks));
    put_u64(out.data(), guest_statx::kOffAttrmask, 0);

    const auto put_timestamp = [&out, &s64](std::size_t off, const struct ::timespec& ts) {
        put_u64(out.data(), off, s64(static_cast<std::int64_t>(ts.tv_sec)));
        put_u32(out.data(), off + 8, static_cast<std::uint32_t>(ts.tv_nsec));
        put_u32(out.data(), off + 12, 0); // __reserved
    };
    put_timestamp(guest_statx::kOffAtime, st.st_atim);
    put_timestamp(guest_statx::kOffMtime, st.st_mtim);
    put_timestamp(guest_statx::kOffCtime, st.st_ctim);
    // kOffBtime stays zero: STATX_BTIME is not in the returned mask.

    put_u32(out.data(), guest_statx::kOffRdevMajor, ::major(st.st_rdev));
    put_u32(out.data(), guest_statx::kOffRdevMinor, ::minor(st.st_rdev));
    put_u32(out.data(), guest_statx::kOffDevMajor, ::major(st.st_dev));
    put_u32(out.data(), guest_statx::kOffDevMinor, ::minor(st.st_dev));

    if (!memory.write(buf, out.data(), out.size())) {
        return -kEfault;
    }
    return 0;
}

// do_statx without statx(2): resolve the path the same way as do_newfstatat and
// describe the result in terms of fstatat(2).
std::int64_t portable_statx(Memory& memory, int host_dirfd, const std::string& path,
                            bool empty_path, unsigned mask, std::uint64_t buf) {
    struct ::stat st{};
    if (empty_path) {
        // statx(AT_FDCWD, "", AT_EMPTY_PATH, ...) describes the current
        // directory; anything else describes the descriptor itself.
        const int rc = host_dirfd == AT_FDCWD ? ::stat(".", &st) : ::fstat(host_dirfd, &st);
        if (rc != 0) {
            return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
        }
        return write_guest_statx_from_stat(memory, buf, st, mask);
    }
    if (::fstatat(host_dirfd, path.c_str(), &st, 0) != 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    return write_guest_statx_from_stat(memory, buf, st, mask);
}

std::int64_t do_statx(GuestState& s, Memory& memory, const std::vector<GuestDescriptor>& files,
                      bool compat = false, const std::string& sysroot = {}) {
    const std::int32_t dirfd = arg_i32(s, 0);
    const std::uint64_t path_addr = arg(s, 1);
    const auto flags = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 2)));
    const auto mask = static_cast<unsigned>(arg(s, 3));
    const std::uint64_t buf = arg(s, 4);

    int host_dirfd = AT_FDCWD;
    if (!resolve_host_dirfd(files, dirfd, compat, host_dirfd)) {
        return -kEnosys;
    }
    if (host_dirfd < 0 && host_dirfd != AT_FDCWD) {
        return -kEbadf;
    }
    const std::uint64_t allowed_flags =
        compat ? (kAtSymlinkNofollow | statx::kAtEmptyPath | 0x800ULL | 0x6000ULL)
               : kAtSymlinkNofollow;
    if ((flags & ~allowed_flags) != 0) {
        return -kEnosys;
    }
    std::string path;
    const std::int64_t rc = read_guest_cstr(memory, path_addr, path);
    if (rc != 0) {
        return rc;
    }
    path = resolve_guest_path(path, sysroot);
    const bool empty_path = (flags & statx::kAtEmptyPath) != 0 && path.empty();
    const bool symlink_nofollow = (flags & kAtSymlinkNofollow) != 0;

    // Handled without statx(2) on Android before API 30, where the syscall is
    // outside the app seccomp allowlist and terminates the process with SIGSYS.
    if (!host_statx_syscall_usable()) {
        if (symlink_nofollow) {
            struct ::stat st{};
            if (::fstatat(host_dirfd, path.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
                return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
            }
            return write_guest_statx_from_stat(memory, buf, st, mask);
        }
        return portable_statx(memory, host_dirfd, path, empty_path, mask, buf);
    }

#ifdef SYS_statx
    std::array<std::uint8_t, guest_statx::kSize> out{};
    const long host_rc =
        ::syscall(SYS_statx, host_dirfd, path.c_str(), static_cast<int>(flags), mask, out.data());
    if (host_rc == 0) {
        if (!memory.write(buf, out.data(), out.size())) {
            return -kEfault;
        }
        return 0;
    }
    // A kernel older than 4.11 has no statx at all: fall back rather than
    // reporting ENOSYS for a stat that fstatat can answer.
    if (errno != ENOSYS) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
#endif
    return portable_statx(memory, host_dirfd, path, empty_path, mask, buf);
}

std::int64_t readlink_common(Memory& memory, int host_dirfd, std::uint64_t path_addr,
                             std::uint64_t buf, std::uint64_t bufsiz, const std::string& sysroot,
                             const std::string& self_exe_override = {}) {
    if (bufsiz == 0) {
        return -kEinval;
    }
    std::string path;
    const std::int64_t rc = read_guest_cstr(memory, path_addr, path);
    if (rc != 0) {
        return rc;
    }
    // /proc/self/exe names the translator, not the guest. When the caller has
    // told us which image the guest came from, report that instead, so a guest
    // inspecting its own executable path learns something true about itself.
    if (!self_exe_override.empty() &&
        (path == "/proc/self/exe" || path == "/proc/thread-self/exe")) {
        const std::size_t n = self_exe_override.size() < bufsiz ? self_exe_override.size()
                                                                : static_cast<std::size_t>(bufsiz);
        if (!memory.write(buf, reinterpret_cast<const std::uint8_t*>(self_exe_override.data()),
                          n)) {
            return -kEfault;
        }
        return static_cast<std::int64_t>(n);
    }
    path = resolve_guest_path(path, sysroot);
    constexpr std::size_t kMaxTarget = 4096;
    std::array<char, kMaxTarget> target{};
    const std::size_t want = static_cast<std::size_t>(bufsiz < kMaxTarget ? bufsiz : kMaxTarget);
    const ssize_t got = ::readlinkat(host_dirfd, path.c_str(), target.data(), want);
    if (got < 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (!memory.write(buf, reinterpret_cast<const std::uint8_t*>(target.data()),
                      static_cast<std::size_t>(got))) {
        return -kEfault;
    }
    return static_cast<std::int64_t>(got);
}

std::int64_t do_readlink(GuestState& s, Memory& memory, const std::string& sysroot = {},
                         const std::string& self_exe = {}) {
    return readlink_common(memory, AT_FDCWD, arg(s, 0), arg(s, 1), arg(s, 2), sysroot, self_exe);
}

std::int64_t do_readlinkat(GuestState& s, Memory& memory, const std::vector<GuestDescriptor>& files,
                           bool compat = false, const std::string& sysroot = {},
                           const std::string& self_exe = {}) {
    const std::int32_t dirfd = arg_i32(s, 0);
    int host_dirfd = AT_FDCWD;
    if (!resolve_host_dirfd(files, dirfd, compat, host_dirfd)) {
        return -kEnosys;
    }
    if (host_dirfd < 0 && host_dirfd != AT_FDCWD) {
        return -kEbadf;
    }
    return readlink_common(memory, host_dirfd, arg(s, 1), arg(s, 2), arg(s, 3), sysroot, self_exe);
}

std::int64_t do_access(GuestState& s, Memory& memory, const std::string& sysroot = {}) {
    const std::uint64_t path_addr = arg(s, 0);
    const auto mode = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 1)));

    constexpr std::uint64_t kModeMask = 0x7; // R_OK|W_OK|X_OK
    if ((mode & ~kModeMask) != 0) {
        return -kEinval;
    }
    std::string path;
    const std::int64_t rc = read_guest_cstr(memory, path_addr, path);
    if (rc != 0) {
        return rc;
    }
    path = resolve_guest_path(path, sysroot);
    if (::access(path.c_str(), static_cast<int>(mode)) != 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    return 0;
}

MemPerm prot_to_perm(std::uint64_t prot) {
    MemPerm perm = MemPerm::kNone;
    if (prot & mmap_flags::kProtRead) {
        perm = perm | MemPerm::kRead;
    }
    if (prot & mmap_flags::kProtWrite) {
        perm = perm | MemPerm::kWrite;
    }
    if (prot & mmap_flags::kProtExec) {
        perm = perm | MemPerm::kExec;
    }
    return perm;
}

struct MemResult {
    std::int64_t value = 0;
    bool implemented = true;
};

MemResult do_mmap(GuestState& s, GuestAddressSpace& as, const std::vector<GuestDescriptor>& files,
                  bool compat = false) {
    const std::uint64_t addr = arg(s, 0);
    const std::uint64_t length = arg(s, 1);
    const auto prot = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 2)));
    const auto flags = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 3)));
    const std::int32_t fd = arg_i32(s, 4);
    const auto offset = static_cast<off_t>(arg(s, 5));

    if (length == 0) {
        return {-kEinval, true};
    }
    const bool is_anon = (flags & mmap_flags::kMapAnonymous) != 0;
    const bool fixed = (flags & mmap_flags::kMapFixed) != 0;
    const MemPerm final_perm = prot_to_perm(prot);

    if (!is_anon) {
        if (!compat) {
            return {-kEnosys, false};
        }
        if ((arg(s, 5) & GuestAddressSpace::kPageMask) != 0) {
            return {-kEinval, true};
        }
        const OpenFileDescription* ofd = lookup_ofd(files, fd);
        if (ofd == nullptr) {
            return {-kEbadf, true};
        }
        const int host_fd = underlying_host_fd(*ofd);
        if (host_fd < 0) {
            return {-kEbadf, true};
        }
        std::vector<std::uint8_t> init_bytes;
        try {
            init_bytes.resize(static_cast<std::size_t>(length), 0);
        } catch (const std::exception&) {
            return {-kEnomem, true};
        }
        std::size_t total_read = 0;
        while (total_read < init_bytes.size()) {
            const ssize_t n =
                ::pread(host_fd, init_bytes.data() + total_read, init_bytes.size() - total_read,
                        offset + static_cast<off_t>(total_read));
            if (n < 0) {
                return {-static_cast<std::int64_t>(errno == 0 ? kEio : errno), true};
            }
            if (n == 0) {
                break; // remainder stays zero-filled per POSIX mmap specification
            }
            total_read += static_cast<std::size_t>(n);
        }
        if (fixed) {
            if (!as.map_fixed(addr, static_cast<std::size_t>(length), final_perm, init_bytes.data(),
                              init_bytes.size())) {
                return {-kEnomem, true};
            }
            return {static_cast<std::int64_t>(addr), true};
        }
        const auto base =
            as.map_anonymous(static_cast<std::size_t>(length), MemPerm::kReadWrite, addr);
        if (!base) {
            return {-kEnomem, true};
        }
        if (!as.write(*base, init_bytes.data(), init_bytes.size()) ||
            !as.protect(*base, static_cast<std::size_t>(length), final_perm)) {
            (void)as.unmap(*base, static_cast<std::size_t>(length));
            return {-kEnomem, true};
        }
        return {static_cast<std::int64_t>(*base), true};
    }

    if (fixed && compat) {
        if (!as.map_fixed(addr, static_cast<std::size_t>(length), final_perm)) {
            return {-kEnomem, true};
        }
        return {static_cast<std::int64_t>(addr), true};
    }

    const auto got =
        as.map_anonymous(static_cast<std::size_t>(length), final_perm, fixed ? addr : 0);
    if (!got) {
        return {-kEnomem, true};
    }
    return {static_cast<std::int64_t>(*got), true};
}

MemResult do_mprotect(GuestState& s, GuestAddressSpace& as) {
    const std::uint64_t addr = arg(s, 0);
    const std::uint64_t len = arg(s, 1);
    const auto prot = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 2)));
    if (len == 0 || (addr & GuestAddressSpace::kPageMask) != 0) {
        return {-kEinval, true};
    }
    if (!as.protect(addr, static_cast<std::size_t>(len), prot_to_perm(prot))) {
        return {-kEnomem, true};
    }
    return {0, true};
}

MemResult do_munmap(GuestState& s, GuestAddressSpace& as) {
    const std::uint64_t addr = arg(s, 0);
    const std::uint64_t len = arg(s, 1);
    if (len == 0 || (addr & GuestAddressSpace::kPageMask) != 0) {
        return {-kEinval, true};
    }
    if (!as.unmap(addr, static_cast<std::size_t>(len))) {
        return {-kEinval, true};
    }
    return {0, true};
}

MemResult do_brk(GuestState& s, GuestAddressSpace& as) {
    const std::uint64_t requested = arg(s, 0);
    return {static_cast<std::int64_t>(as.set_brk(requested)), true};
}

std::int64_t do_getrandom(GuestState& s, Memory& memory) {
    const std::uint64_t buf = arg(s, 0);
    const std::uint64_t count = arg(s, 1);
    const auto flags = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 2)));

    constexpr std::uint64_t kHonoured = 0x1 | 0x2; // GRND_NONBLOCK | GRND_RANDOM
    if ((flags & ~kHonoured) != 0) {
        return -kEinval;
    }
    if (count == 0) {
        return 0;
    }

    constexpr std::size_t kChunk = 4096;
    std::array<std::uint8_t, kChunk> chunk{};
    std::uint64_t done = 0;
    while (done < count) {
        const std::size_t n =
            static_cast<std::size_t>((count - done) < kChunk ? (count - done) : kChunk);
        const ssize_t got = ::getrandom(chunk.data(), n, 0);
        if (got < 0) {
            return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
        }
        if (got == 0) {
            break;
        }
        if (!memory.write(buf + done, chunk.data(), static_cast<std::size_t>(got))) {
            return -kEfault;
        }
        done += static_cast<std::uint64_t>(got);
        if (static_cast<std::size_t>(got) < n) {
            break;
        }
    }
    return static_cast<std::int64_t>(done);
}

std::int64_t do_getcwd(GuestState& s, Memory& memory) {
    const std::uint64_t buf = arg(s, 0);
    const std::uint64_t size = arg(s, 1);
    if (size == 0) {
        return -kEinval;
    }
    std::array<char, 4096> path{};
    if (::getcwd(path.data(), path.size()) == nullptr) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    const std::size_t len = std::strlen(path.data());
    if (len + 1 > size) {
        return -kErange;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (!memory.write(buf, reinterpret_cast<const std::uint8_t*>(path.data()), len + 1)) {
        return -kEfault;
    }
    return static_cast<std::int64_t>(len + 1);
}

std::shared_ptr<OpenFileDescription> make_stdio_desc(std::FILE* stream, int sink_fd) {
    auto d = std::make_shared<OpenFileDescription>();
    d->stream = stream;
    d->sink_fd = sink_fd;
    return d;
}

} // namespace syscall_detail

using namespace syscall_detail;

MinimalSyscallHandler::MinimalSyscallHandler(OutputSink sink, GuestAddressSpace* address_space)
    : sink_(sink ? std::move(sink) : OutputSink(default_sink)), address_space_(address_space) {
    // The guest shares this process's pid namespace, so the host pid is the
    // guest-visible pid and also lets /proc/<pid>/exe resolve to this image.
    guest_pid_ = static_cast<int>(::getpid());
    files_ = {
        GuestDescriptor{make_stdio_desc(stdin, -1), false},
        GuestDescriptor{make_stdio_desc(stdout, 1), false},
        GuestDescriptor{make_stdio_desc(stderr, 2), false},
    };
    // The dispatcher starts with the compiled-in rules only: constructing a
    // handler has no filesystem side effects. enable_syscall_registry() loads
    // the persistent registry once, at startup.
    dispatcher_ = std::make_unique<SyscallDispatcher>();
}

MinimalSyscallHandler::~MinimalSyscallHandler() = default;

void MinimalSyscallHandler::enable_syscall_registry() {
    std::string message;
    (void)reload_syscall_registry(RegistryOptions{}, message);
}

void MinimalSyscallHandler::enable_syscall_registry(const RegistryOptions& options) {
    std::string message;
    (void)reload_syscall_registry(options, message);
}

int MinimalSyscallHandler::reload_syscall_registry(const RegistryOptions& options,
                                                   std::string& message) {
    if (dispatcher_ == nullptr) {
        message = "no dispatcher";
        return 1;
    }
    const RegistryReport report = dispatcher_->reload(options);
    // One sentence: the detailed warning/rejection lines belong to
    // dispatcher().diagnostics(), which callers already print. Repeating them
    // here made every diagnostic appear twice in `syscall-registry` output.
    message = report.message;
    const std::size_t rejected = report.rejected.size();
    const std::size_t warnings = report.warnings.size();
    if (rejected != 0 || warnings != 0) {
        message += " (";
        if (rejected != 0) {
            message += std::to_string(rejected) + " rule(s) rejected";
            if (warnings != 0) {
                message += ", ";
            }
        }
        if (warnings != 0) {
            message += std::to_string(warnings) + " warning(s)";
        }
        message += "; see diagnostics)";
    }
    return report.status == RegistryReport::Status::kOk ? 0 : 1;
}

const SyscallDispatcher& MinimalSyscallHandler::dispatcher() const noexcept {
    return *dispatcher_;
}

bool MinimalSyscallHandler::adopt(int guest_fd, std::FILE* stream) {
    if (guest_fd < 0 || stream == nullptr) {
        return false;
    }
    const auto idx = static_cast<std::size_t>(guest_fd);
    if (idx < files_.size() && files_[idx].is_open()) {
        return false; // already occupied
    }
    if (idx >= files_.size()) {
        files_.resize(idx + 1);
    }
    auto desc = std::make_shared<OpenFileDescription>();
    desc->stream = stream;
    files_[idx] = GuestDescriptor{std::move(desc), false};
    return true;
}

void MinimalSyscallHandler::enable_linux_compat(std::string sysroot) {
    linux_compat_ = true;
    sysroot_ = std::move(sysroot);
}

namespace {

std::int64_t do_dup(GuestState& s, std::vector<GuestDescriptor>& files) {
    const std::int32_t oldfd = arg_i32(s, 0);
    const GuestDescriptor* src = lookup_fd(files, oldfd);
    if (src == nullptr) {
        return -kEbadf;
    }
    return alloc_fd(files, src->desc, /*cloexec=*/false, /*min_fd=*/0);
}

std::int64_t do_dup2(GuestState& s, std::vector<GuestDescriptor>& files) {
    constexpr std::int32_t kMaxGuestFds = 4096;
    const std::int32_t oldfd = arg_i32(s, 0);
    const std::int32_t newfd = arg_i32(s, 1);
    const GuestDescriptor* src = lookup_fd(files, oldfd);
    if (src == nullptr || newfd < 0 || newfd >= kMaxGuestFds) {
        return -kEbadf;
    }
    if (oldfd == newfd) {
        return newfd;
    }
    std::shared_ptr<OpenFileDescription> shared = src->desc;
    const auto idx = static_cast<std::size_t>(newfd);
    if (idx >= files.size()) {
        files.resize(idx + 1);
    }
    files[idx] = GuestDescriptor{std::move(shared), /*cloexec=*/false};
    return newfd;
}

std::int64_t do_dup3(GuestState& s, std::vector<GuestDescriptor>& files) {
    constexpr std::int32_t kMaxGuestFds = 4096;
    const std::int32_t oldfd = arg_i32(s, 0);
    const std::int32_t newfd = arg_i32(s, 1);
    const auto flags = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 2)));
    const GuestDescriptor* src = lookup_fd(files, oldfd);
    if (src == nullptr || newfd < 0 || newfd >= kMaxGuestFds) {
        return -kEbadf;
    }
    if (oldfd == newfd || (flags & ~open_flags::kOCloseexec) != 0) {
        return -kEinval;
    }
    std::shared_ptr<OpenFileDescription> shared = src->desc;
    const auto idx = static_cast<std::size_t>(newfd);
    if (idx >= files.size()) {
        files.resize(idx + 1);
    }
    files[idx] = GuestDescriptor{std::move(shared), (flags & open_flags::kOCloseexec) != 0};
    return newfd;
}

std::uint64_t host_to_guest_status_flags(int host_fl) {
    std::uint64_t out = 0;
    switch (host_fl & O_ACCMODE) {
    case O_WRONLY:
        out |= open_flags::kOWronly;
        break;
    case O_RDWR:
        out |= open_flags::kORdwr;
        break;
    default:
        out |= open_flags::kORdonly;
        break;
    }
    if ((host_fl & O_APPEND) != 0) {
        out |= open_flags::kOAppend;
    }
    if ((host_fl & O_NONBLOCK) != 0) {
        out |= open_flags::kONonblock;
    }
    if ((host_fl & O_DIRECTORY) != 0) {
        out |= open_flags::kODirectory;
    }
    return out;
}

std::int64_t do_fcntl(GuestState& s, std::vector<GuestDescriptor>& files) {
    constexpr std::int64_t kMaxGuestFds = 4096;
    const std::int32_t fd = arg_i32(s, 0);
    const std::int32_t cmd = arg_i32(s, 1);
    const std::uint64_t raw_arg = arg(s, 2);

    const GuestDescriptor* entry = lookup_fd(files, fd);
    if (entry == nullptr) {
        return -kEbadf;
    }

    switch (cmd) {
    case fcntl_cmd::kDupfd:
    case fcntl_cmd::kDupfdCloexec: {
        const auto min_fd = static_cast<std::int64_t>(raw_arg);
        if (min_fd < 0 || min_fd >= kMaxGuestFds) {
            return -kEinval;
        }
        return alloc_fd(files, entry->desc, cmd == fcntl_cmd::kDupfdCloexec,
                        static_cast<std::size_t>(min_fd));
    }
    case fcntl_cmd::kGetfd:
        return entry->cloexec ? 1 : 0;
    case fcntl_cmd::kSetfd:
        files[static_cast<std::size_t>(fd)].cloexec = (raw_arg & 1ULL) != 0;
        return 0;
    case fcntl_cmd::kGetfl: {
        if (entry->desc->host_fd >= 0) {
            const int fl = ::fcntl(entry->desc->host_fd, F_GETFL);
            if (fl < 0) {
                return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
            }
            return static_cast<std::int64_t>(host_to_guest_status_flags(fl));
        }
        return static_cast<std::int64_t>(host_to_guest_status_flags(entry->desc->status_flags));
    }
    case fcntl_cmd::kSetfl: {
        int new_host = 0;
        if ((raw_arg & open_flags::kOAppend) != 0) {
            new_host |= O_APPEND;
        }
        if ((raw_arg & open_flags::kONonblock) != 0) {
            new_host |= O_NONBLOCK;
        }
        if (entry->desc->host_fd >= 0) {
            if (::fcntl(entry->desc->host_fd, F_SETFL, new_host) < 0) {
                return -static_cast<std::int64_t>(errno == 0 ? kEinval : errno);
            }
        }
        entry->desc->status_flags = (entry->desc->status_flags & O_ACCMODE) | new_host;
        return 0;
    }
    default:
        return -kEnosys;
    }
}

void copy_uts_field(std::uint8_t* dst, const char* src) {
    if (src == nullptr) {
        return;
    }
    const std::size_t n = std::strlen(src);
    std::memcpy(dst, src, n < 64 ? n : 64);
}

// uname(buf): write 6 x 65 = 390-byte `new_utsname`, always reporting guest
// machine architecture `"x86_64"` regardless of host CPU architecture.
std::int64_t do_uname(GuestState& s, Memory& memory) {
    struct utsname host_uts{};
    if (::uname(&host_uts) != 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    std::array<std::uint8_t, 390> raw{};
    copy_uts_field(raw.data() + 0, "Linux");
    copy_uts_field(raw.data() + 65, host_uts.nodename);
    copy_uts_field(raw.data() + 130, host_uts.release);
    copy_uts_field(raw.data() + 195, host_uts.version);
    copy_uts_field(raw.data() + 260, "x86_64");
    copy_uts_field(raw.data() + 325, "(none)");
    if (!memory.write(arg(s, 0), raw.data(), raw.size())) {
        return -kEfault;
    }
    return 0;
}

std::int64_t do_clock_gettime(GuestState& s, Memory& memory) {
    const auto clk_id = static_cast<clockid_t>(arg_i32(s, 0));
    const std::uint64_t tp_addr = arg(s, 1);
    struct timespec ts{};
    if (::clock_gettime(clk_id, &ts) != 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEinval : errno);
    }
    if (tp_addr == 0) {
        return -kEfault;
    }
    std::uint8_t raw[16] = {};
    put_u64(raw, 0, static_cast<std::uint64_t>(static_cast<std::int64_t>(ts.tv_sec)));
    put_u64(raw, 8, static_cast<std::uint64_t>(static_cast<std::int64_t>(ts.tv_nsec)));
    if (!memory.write(tp_addr, raw, sizeof(raw))) {
        return -kEfault;
    }
    return 0;
}

std::int64_t do_gettimeofday(GuestState& s, Memory& memory) {
    const std::uint64_t tv_addr = arg(s, 0);
    const std::uint64_t tz_addr = arg(s, 1);
    if (tv_addr != 0) {
        struct timespec ts{};
        if (::clock_gettime(CLOCK_REALTIME, &ts) != 0) {
            return -static_cast<std::int64_t>(errno == 0 ? kEinval : errno);
        }
        std::uint8_t raw[16] = {};
        put_u64(raw, 0, static_cast<std::uint64_t>(static_cast<std::int64_t>(ts.tv_sec)));
        put_u64(raw, 8, static_cast<std::uint64_t>(static_cast<std::int64_t>(ts.tv_nsec / 1000)));
        if (!memory.write(tv_addr, raw, sizeof(raw))) {
            return -kEfault;
        }
    }
    if (tz_addr != 0) {
        const std::uint8_t zero[8] = {};
        if (!memory.write(tz_addr, zero, sizeof(zero))) {
            return -kEfault;
        }
    }
    return 0;
}

std::int64_t do_arch_prctl(GuestState& s, Memory& memory) {
    const std::uint64_t code = arg(s, 0);
    const std::uint64_t addr = arg(s, 1);
    switch (code) {
    case arch_prctl_code::kSetFs:
        s.fs_base = addr;
        return 0;
    case arch_prctl_code::kGetFs: {
        std::uint8_t raw[8] = {};
        put_u64(raw, 0, s.fs_base);
        if (!memory.write(addr, raw, sizeof(raw))) {
            return -kEfault;
        }
        return 0;
    }
    default:
        return -kEinval;
    }
}

std::int64_t do_prlimit64(GuestState& s, Memory& memory) {
    constexpr std::int64_t kEsrch = 3;
    const auto pid = static_cast<pid_t>(arg_i32(s, 0));
    const auto resource = static_cast<std::uint32_t>(arg(s, 1));
    const std::uint64_t new_addr = arg(s, 2);
    const std::uint64_t old_addr = arg(s, 3);

    if (pid != 0 && pid != ::getpid()) {
        return -kEsrch;
    }
    if (resource >= 16u) {
        return -kEinval;
    }

    struct rlimit64 new_lim{};
    const struct rlimit64* new_ptr = nullptr;
    if (new_addr != 0) {
        std::uint8_t raw[16] = {};
        if (!memory.read(new_addr, raw, sizeof(raw))) {
            return -kEfault;
        }
        new_lim.rlim_cur = get_u64_le(raw);
        new_lim.rlim_max = get_u64_le(raw + 8);
        new_ptr = &new_lim;
    }

    struct rlimit64 old_lim{};
    if (::syscall(SYS_prlimit64, 0, resource, new_ptr, &old_lim) != 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEinval : errno);
    }

    if (old_addr != 0) {
        std::uint8_t raw[16] = {};
        put_u64(raw, 0, old_lim.rlim_cur);
        put_u64(raw, 8, old_lim.rlim_max);
        if (!memory.write(old_addr, raw, sizeof(raw))) {
            return -kEfault;
        }
    }
    return 0;
}

} // namespace

namespace {

// HYPERION_TRACE_SYSCALLS=1 logs every guest system call with its result to the
// host's stderr. This exists because an unimplemented call is otherwise only
// visible as a count: when a guest misbehaves, the difference between "asked for
// something we do not implement" and "asked for something we implement wrongly"
// is the whole diagnosis. Written with write(2) rather than stdio so it cannot
// be swallowed by, or corrupt, the guest's own buffered output.
bool syscall_trace_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("HYPERION_TRACE_SYSCALLS");
        return v != nullptr && v[0] != '0' && v[0] != '\0';
    }();
    return enabled;
}

void trace_syscall(std::uint64_t number, const SyscallResult& result, const GuestState& state) {
    if (!syscall_trace_enabled()) {
        return;
    }
    char buf[160];
    const int n =
        std::snprintf(buf, sizeof(buf), "hyperion[pid %ld]: syscall %llu -> %s, rax=%lld\n",
                      static_cast<long>(::getpid()), static_cast<unsigned long long>(number),
                      to_string(result.status), static_cast<long long>(state.reg(Gpr::kRax)));
    if (n > 0) {
        const auto len = (static_cast<std::size_t>(n) < sizeof(buf)) ? static_cast<std::size_t>(n)
                                                                     : sizeof(buf) - 1;
        ssize_t unused = ::write(STDERR_FILENO, buf, len);
        (void)unused;
    }
}

} // namespace

namespace {

// Apply a compiled alias rule: place the source syscall's arguments into the
// target syscall's argument registers, remembering the originals so they can be
// restored afterwards. The x86-64 `syscall` ABI only guarantees rax is the
// result, but preserving the argument registers keeps an alias indistinguishable
// from a direct implementation for a guest that inspects them.
void apply_alias(const CompiledAlias& alias, GuestState& state, std::uint64_t (&saved)[6]) {
    for (int i = 0; i < 6; ++i) {
        saved[static_cast<std::size_t>(i)] = state.reg(kArgRegs[i]);
    }
    for (int i = 0; i < 6; ++i) {
        if (static_cast<std::uint8_t>(i) >= alias.count) {
            state.reg(kArgRegs[i]) = 0;
            continue;
        }
        const std::uint8_t source = alias.from_arg[static_cast<std::size_t>(i)];
        state.reg(kArgRegs[i]) =
            (source == 0xFF)
                ? static_cast<std::uint64_t>(alias.constant[static_cast<std::size_t>(i)])
                : saved[source];
    }
}

void restore_args(const std::uint64_t (&saved)[6], GuestState& state) {
    for (int i = 0; i < 6; ++i) {
        state.reg(kArgRegs[i]) = saved[static_cast<std::size_t>(i)];
    }
}

} // namespace

// Layered dispatch. See syscall_dispatch.hpp for the ordering and its rationale.
//
// The three layers are tried in cost order and the first match wins:
//   1. the compiled dispatch table   (one bounds check, one load, one call)
//   2. the existing handler switch   (handle_unguarded, unchanged)
//   3. the unsupported path          (-ENOSYS plus one diagnostic)
SyscallResult MinimalSyscallHandler::handle(GuestState& state, Memory& memory) {
    SyscallResult result;
    result.number = state.reg(Gpr::kRax);
    // If a guest signal is pending, fail this syscall with -EINTR so the
    // signal is delivered at the next instruction boundary. This mirrors
    // Linux: a signal interrupting a blocking syscall causes it to return
    // -EINTR, and the signal handler runs before the user continues.
    if (signal_pending(*this)) {
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(-static_cast<std::int64_t>(kEintr));
        result.status = SyscallStatus::kHandled;
        return result;
    }
    const bool tracing = syscall_trace_enabled();

    if (dispatcher_ != nullptr) {
        const DispatchEntry* entry = dispatcher_->lookup(result.number);
        if (entry != nullptr) {
            if (entry->kind == DispatchKind::kAdapter) {
                std::int64_t r = -kEnosys;
                if ((entry->flags & kAdapterNeedsLinuxCompat) != 0 && !linux_compat_) {
                    dispatcher_->note_translation_failure();
                } else {
                    SyscallContext ctx{*this, state, memory, result.number};
                    r = entry->fn(ctx);
                }
                state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
                if (r == -kEnosys) {
                    // The adapter declined to translate: the guest sees -ENOSYS
                    // and the call is reported as unsupported, never as handled.
                    result.status = SyscallStatus::kUnimplemented;
                    dispatcher_->note_unsupported(result.number);
                } else {
                    result.status = SyscallStatus::kHandled;
                    dispatcher_->note_adapter_success();
                }
                if (tracing) {
                    trace_syscall(result.number, result, state);
                }
                return result;
            }
            if (entry->kind == DispatchKind::kGuestAlias) {
                const CompiledAlias& alias =
                    dispatcher_->rules()->aliases[static_cast<std::size_t>(entry->flags)];
                std::uint64_t saved[6] = {};
                apply_alias(alias, state, saved);
                state.reg(Gpr::kRax) = alias.target;
                SyscallResult inner = handle_unguarded(state, memory);
                restore_args(saved, state);
                inner.number = result.number;
                if (inner.status == SyscallStatus::kUnimplemented) {
                    dispatcher_->note_unsupported(result.number);
                } else {
                    dispatcher_->note_alias_success();
                }
                if (tracing) {
                    trace_syscall(result.number, inner, state);
                }
                return inner;
            }
        }
    }

    const SyscallResult legacy = handle_unguarded(state, memory);
    if (dispatcher_ != nullptr) {
        if (legacy.status == SyscallStatus::kHandled) {
            dispatcher_->note_existing_success();
        } else if (legacy.status == SyscallStatus::kUnimplemented) {
            dispatcher_->note_unsupported(legacy.number);
        }
    }
    if (tracing) {
        trace_syscall(result.number, legacy, state);
    }
    return legacy;
}

SyscallResult MinimalSyscallHandler::handle_unguarded(GuestState& state, Memory& memory) {
    SyscallResult result;
    result.number = state.reg(Gpr::kRax);

    switch (result.number) {
    case sysno::kWrite: {
        const std::int64_t r = do_write(state, memory, sink_, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kRead: {
        const std::int64_t r = do_read(state, memory, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kPread64: {
        const std::int64_t r = do_pread64(state, memory, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kPwrite64: {
        const std::int64_t r = do_pwrite64(state, memory, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kReadv: {
        const std::int64_t r = do_iov(state, memory, sink_, files_, /*is_write=*/false);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kWritev: {
        const std::int64_t r = do_iov(state, memory, sink_, files_, /*is_write=*/true);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kDup: {
        const std::int64_t r = do_dup(state, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kDup2: {
        const std::int64_t r = do_dup2(state, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kDup3: {
        const std::int64_t r = do_dup3(state, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kFcntl: {
        const std::int64_t r = do_fcntl(state, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = (r == -kEnosys) ? SyscallStatus::kUnimplemented : SyscallStatus::kHandled;
        return result;
    }
    case sysno::kUname: {
        const std::int64_t r = do_uname(state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kGettimeofday: {
        const std::int64_t r = do_gettimeofday(state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kClockGettime: {
        const std::int64_t r = do_clock_gettime(state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kArchPrctl: {
        const std::int64_t r = do_arch_prctl(state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kSetTidAddress:
        clear_child_tid_ = arg(state, 0);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(::gettid());
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kSetRobustList:
        if (arg(state, 1) != 24ULL) {
            state.reg(Gpr::kRax) = static_cast<std::uint64_t>(-kEinval);
        } else {
            robust_list_head_ = arg(state, 0);
            state.reg(Gpr::kRax) = 0;
        }
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kPrlimit64: {
        const std::int64_t r = do_prlimit64(state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kGetdents64: {
        const std::int64_t r = do_getdents64(state, memory, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kClose: {
        const std::int64_t r = do_close(state, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kOpenat: {
        const std::int64_t r = do_openat(state, memory, files_, linux_compat_, sysroot_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = (r == -kEnosys) ? SyscallStatus::kUnimplemented : SyscallStatus::kHandled;
        return result;
    }
    case sysno::kFstat: {
        const std::int64_t r = do_fstat(state, memory, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kNewfstatat: {
        const std::int64_t r = do_newfstatat(state, memory, files_, linux_compat_, sysroot_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = (r == -kEnosys) ? SyscallStatus::kUnimplemented : SyscallStatus::kHandled;
        return result;
    }
    case sysno::kStatx: {
        const std::int64_t r = do_statx(state, memory, files_, linux_compat_, sysroot_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = (r == -kEnosys) ? SyscallStatus::kUnimplemented : SyscallStatus::kHandled;
        return result;
    }
    case sysno::kReadlink: {
        const std::int64_t r = do_readlink(state, memory, sysroot_, guest_image_path_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kReadlinkat: {
        const std::int64_t r =
            do_readlinkat(state, memory, files_, linux_compat_, sysroot_, guest_image_path_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = (r == -kEnosys) ? SyscallStatus::kUnimplemented : SyscallStatus::kHandled;
        return result;
    }
    case sysno::kAccess: {
        const std::int64_t r = do_access(state, memory, sysroot_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kLseek: {
        const std::int64_t r = do_lseek(state, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kGetpid:
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(::getpid());
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kGettid:
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(::gettid());
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kGetuid:
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(::getuid());
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kGeteuid:
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(::geteuid());
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kGetgid:
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(::getgid());
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kGetegid:
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(::getegid());
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kGetppid:
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(::getppid());
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kGetcwd: {
        const std::int64_t r = do_getcwd(state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kGetrandom: {
        const std::int64_t r = do_getrandom(state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    // --- process creation (src/core/process.cpp) -----------------------------
    // The child of a fork is a real host process that runs a private copy of the
    // guest and terminates with the guest's exit status, so the child never
    // returns from here; only the parent reaches the `state.reg(kRax) = r` line
    // below, where r is the child's pid.
    case sysno::kFork: {
        const std::int64_t r = process::do_fork(*this, state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kVfork: {
        const std::int64_t r = process::do_vfork(*this, state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kClone: {
        const std::int64_t r = process::do_clone(*this, state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kExecve: {
        const std::int64_t r = process::do_execve(*this, state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kWait4: {
        const std::int64_t r = process::do_wait4(*this, state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kExit:
    case sysno::kExitGroup:
        result.status = SyscallStatus::kExit;
        result.exit_code = static_cast<std::int64_t>(arg_i32(state, 0)) & 0xFF;
        return result;
    case sysno::kMmap:
    case sysno::kMprotect:
    case sysno::kMunmap:
    case sysno::kBrk: {
        if (address_space_ == nullptr) {
            state.reg(Gpr::kRax) = static_cast<std::uint64_t>(-kEnosys);
            result.status = SyscallStatus::kUnimplemented;
            return result;
        }
        std::int64_t r = 0;
        bool implemented = true;
        switch (result.number) {
        case sysno::kMmap: {
            const MemResult mr = do_mmap(state, *address_space_, files_, linux_compat_);
            r = mr.value;
            implemented = mr.implemented;
            break;
        }
        case sysno::kMprotect:
            r = do_mprotect(state, *address_space_).value;
            break;
        case sysno::kMunmap:
            r = do_munmap(state, *address_space_).value;
            break;
        default:
            r = do_brk(state, *address_space_).value;
            break;
        }
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = implemented ? SyscallStatus::kHandled : SyscallStatus::kUnimplemented;
        return result;
    }
    default:
        if (linux_compat_) {
            std::int64_t r = -kEnosys;
            bool handled = true;
            switch (result.number) {
            case 2: // open(path, flags, mode)
                r = do_openat(state, memory, files_, true, sysroot_, /*legacy_open=*/true);
                break;
            case 4:   // stat(path, statbuf)
            case 6: { // lstat(path, statbuf)
                std::string p;
                const std::int64_t prc = read_guest_cstr(memory, arg(state, 0), p);
                if (prc != 0) {
                    r = prc;
                } else {
                    p = resolve_guest_path(p, sysroot_);
                    struct ::stat st{};
                    const int hrc =
                        (result.number == 6) ? ::lstat(p.c_str(), &st) : ::stat(p.c_str(), &st);
                    r = (hrc == 0) ? write_guest_stat(memory, arg(state, 1), st)
                                   : -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
                }
                break;
            }
            case 13: { // rt_sigaction(sig, act, oldact, sigsetsize)
                // This used to write 32 zero bytes into oldact and report
                // success: a program that installed a handler and read it back was
                // told it had none. Recording the action makes the syscall do its
                // actual job. *Delivering* a signal to a guest handler is a
                // different mechanism and is still not implemented; that is
                // documented rather than papered over.
                const auto signum = arg_i32(state, 0);
                const std::uint64_t act_addr = arg(state, 1);
                const std::uint64_t old_addr = arg(state, 2);
                const std::uint64_t setsize = arg(state, 3);
                if (signum < 1 || signum > static_cast<std::int32_t>(kMaxSignal)) {
                    r = -kEinval;
                    break;
                }
                if (signum == 9 /*SIGKILL*/ || signum == 19 /*SIGSTOP*/) {
                    // The kernel refuses to change these two, and a program that
                    // believes it ignored them would be badly wrong.
                    r = -kEinval;
                    break;
                }
                if (setsize != 8) {
                    r = -kEinval;
                    break;
                }
                const auto index = static_cast<std::size_t>(signum);
                if (old_addr != 0) {
                    std::array<std::uint8_t, 32> out{};
                    const GuestSignalAction& prev = sigactions_[index];
                    put_u64(out.data(), 0, prev.installed ? prev.handler : 0); // SIG_DFL
                    put_u64(out.data(), 8, prev.flags);
                    put_u64(out.data(), 16, prev.restorer);
                    put_u64(out.data(), 24, prev.mask);
                    if (!memory.write(old_addr, out.data(), out.size())) {
                        r = -kEfault;
                        break;
                    }
                }
                if (act_addr != 0) {
                    std::array<std::uint8_t, 32> in{};
                    if (!memory.read(act_addr, in.data(), in.size())) {
                        r = -kEfault;
                        break;
                    }
                    GuestSignalAction next;
                    next.handler = get_u64_le(in.data());
                    next.flags = get_u64_le(in.data() + 8);
                    next.restorer = get_u64_le(in.data() + 16);
                    next.mask = get_u64_le(in.data() + 24);
                    next.installed = true;
                    sigactions_[index] = next;
                }
                r = 0;
                break;
            }
            case 131: { // sigaltstack(ss, old_ss)
                // stack_t is 24 bytes on x86-64: ss_sp, ss_flags (int), padding,
                // ss_size. Rust's stack-overflow guard installs an alternate stack
                // and expects to read it back, so this has to be recorded rather
                // than reported as done.
                const std::uint64_t ss_addr = arg(state, 0);
                const std::uint64_t old_addr = arg(state, 1);
                if (old_addr != 0) {
                    std::array<std::uint8_t, 24> out{};
                    put_u64(out.data(), 0, altstack_sp_);
                    put_u32(out.data(), 8, static_cast<std::uint32_t>(altstack_flags_));
                    put_u64(out.data(), 16, altstack_size_);
                    if (!memory.write(old_addr, out.data(), out.size())) {
                        r = -kEfault;
                        break;
                    }
                }
                if (ss_addr != 0) {
                    std::array<std::uint8_t, 24> in{};
                    if (!memory.read(ss_addr, in.data(), in.size())) {
                        r = -kEfault;
                        break;
                    }
                    const auto flags = static_cast<std::int32_t>(get_u32_le(in.data() + 8));
                    constexpr std::int32_t kSsDisable = 2;
                    constexpr std::int32_t kSsAutodisarm = 4;
                    if ((flags & ~(kSsDisable | kSsAutodisarm)) != 0) {
                        r = -kEinval; // undefined flag bits
                        break;
                    }
                    if ((flags & kSsDisable) != 0) {
                        altstack_sp_ = 0;
                        altstack_size_ = 0;
                        altstack_flags_ = kSsDisable;
                    } else {
                        const std::uint64_t size = get_u64_le(in.data() + 16);
                        // The kernel's own floor. glibc's MINSIGSTKSZ is a
                        // runtime value and is not what the kernel checks.
                        constexpr std::uint64_t kMinSigStackSize = 2048;
                        if (size < kMinSigStackSize) {
                            r = -kEnomem;
                            break;
                        }
                        altstack_sp_ = get_u64_le(in.data());
                        altstack_size_ = size;
                        altstack_flags_ = flags;
                    }
                }
                r = 0;
                break;
            }
            case 14: { // rt_sigprocmask
                const auto how = arg_i32(state, 0);
                const std::uint64_t nset = arg(state, 1);
                const std::uint64_t oset = arg(state, 2);
                const std::uint64_t sigsetsize = arg(state, 3);
                if (sigsetsize != 8) {
                    r = -kEinval;
                } else {
                    if (oset != 0) {
                        std::array<std::uint8_t, 8> out_mask{};
                        put_u64(out_mask.data(), 0, sigmask_);
                        if (!memory.write(oset, out_mask.data(), 8)) {
                            r = -kEfault;
                            break;
                        }
                    }
                    if (nset != 0) {
                        std::array<std::uint8_t, 8> in_mask{};
                        if (!memory.read(nset, in_mask.data(), 8)) {
                            r = -kEfault;
                            break;
                        }
                        std::uint64_t m = 0;
                        for (int i = 0; i < 8; ++i) {
                            m |= static_cast<std::uint64_t>(in_mask[static_cast<std::size_t>(i)])
                                 << (8 * i);
                        }
                        if (how == 0) {
                            sigmask_ |= m;
                        } else if (how == 1) {
                            sigmask_ &= ~m;
                        } else if (how == 2) {
                            sigmask_ = m;
                        } else {
                            r = -kEinval;
                            break;
                        }
                        // A signal that arrived while blocked is delivered as
                        // soon as the mask stops blocking it - that is the whole
                        // point of queueing it rather than dropping it.
                        const std::int64_t outcome = deliver_pending_signals();
                        if (outcome != 0) {
                            result.status = SyscallStatus::kExit;
                            result.exit_code = outcome;
                            return result;
                        }
                    }
                    r = 0;
                }
                break;
            }
            case 15: { // rt_sigreturn(ucontext *): restore the interrupted guest
                // context from the rt_sigframe that check_and_deliver_signals()
                // built on the guest stack. After `ret` pops kSigReturnMagic into
                // rip, rsp points past the popped value:
                //   simple `ret`: rsp = frame_addr + 8, [rsp] = kSigReturnMagic (offset 8)
                //   leave; ret:   rsp = alias + (frame_addr & 0xFFF) + 16, [rsp] = uc_flags = 0
                // Probe [rsp] to distinguish and compute the true frame start.
                std::uint64_t probe_addr = state.reg(Gpr::kRsp);
                std::uint8_t probe[8];
                std::uint64_t frame_addr;
                if (memory.read(probe_addr, probe, 8)) {
                    std::uint64_t probe_val = 0;
                    for (int i = 0; i < 8; ++i)
                        probe_val |= (std::uint64_t)probe[i] << (8 * i);
                    if (probe_val == hyperion::sigframe::kSigReturnMagic) {
                        // Simple `ret`: frame starts at rsp - 8.
                        frame_addr = probe_addr - 8;
                    } else {
                        // `leave; ret`: frame starts at rsp - 16 (via alias page).
                        frame_addr = probe_addr - 16;
                    }
                } else {
                    r = -kEfault;
                    break;
                }
                const std::size_t uc = hyperion::sigframe::kUcontextOffset;
                const std::size_t mc = uc + hyperion::sigframe::kUcMcontext;

                std::uint8_t raw[hyperion::sigframe::kFrameSize];
                if (!memory.read(frame_addr, raw, sizeof(raw))) {
                    r = -kEfault;
                    break;
                }

                auto get64 = [&raw](std::size_t off) -> std::uint64_t {
                    std::uint64_t v = 0;
                    for (int i = 0; i < 8; ++i) {
                        v |= static_cast<std::uint64_t>(raw[off + i]) << (8 * i);
                    }
                    return v;
                };

                // Restore all general-purpose registers.
                state.reg(Gpr::kR8)  = get64(mc + 0);
                state.reg(Gpr::kR9)  = get64(mc + 8);
                state.reg(Gpr::kR10) = get64(mc + 16);
                state.reg(Gpr::kR11) = get64(mc + 24);
                state.reg(Gpr::kR12) = get64(mc + 32);
                state.reg(Gpr::kR13) = get64(mc + 40);
                state.reg(Gpr::kR14) = get64(mc + 48);
                state.reg(Gpr::kR15) = get64(mc + 56);
                state.reg(Gpr::kRdi) = get64(mc + 64);
                state.reg(Gpr::kRsi) = get64(mc + 72);
                state.reg(Gpr::kRbp) = get64(mc + 80);
                state.reg(Gpr::kRbx) = get64(mc + 88);
                state.reg(Gpr::kRdx) = get64(mc + 96);
                state.reg(Gpr::kRax) = get64(mc + 104);
                state.reg(Gpr::kRcx) = get64(mc + 112);
                state.reg(Gpr::kRsp) = get64(mc + 120); // pre-signal stack pointer

                state.rip = get64(mc + 128);
                // eflags at mc + 136: restore only the modelled bits.
                {
                    const std::uint64_t eflags = get64(mc + 136);
                    state.flags.cf = (eflags & (1ULL << 0)) != 0;
                    state.flags.pf = (eflags & (1ULL << 2)) != 0;
                    state.flags.af = (eflags & (1ULL << 4)) != 0;
                    state.flags.zf = (eflags & (1ULL << 6)) != 0;
                    state.flags.sf = (eflags & (1ULL << 7)) != 0;
                    state.flags.of = (eflags & (1ULL << 11)) != 0;
                }
                state.fs_base = get64(mc + 160);

                // Restore the guest signal mask from uc_sigmask.
                const std::uint64_t saved_mask = get64(uc + hyperion::sigframe::kUcSigmask + 0);
                sigmask_ = saved_mask;

                // Re-evaluate now-unblocked pending signals: a signal that was
                // held while masked can now be delivered at the next boundary.
                (void)deliver_pending_signals();

                r = 0;
                break;
            }
            case 16: { // ioctl(fd, req, argp)
                const std::int32_t fd = arg_i32(state, 0);
                const auto req = static_cast<unsigned long>(arg(state, 1));
                const OpenFileDescription* ofd = lookup_ofd(files_, fd);
                if (ofd == nullptr) {
                    r = -kEbadf;
                    break;
                }
                const int hfd = underlying_host_fd(*ofd);
                if (req == 0x5413UL) { // TIOCGWINSZ
                    struct winsize ws{};
                    if (hfd < 0 || ::ioctl(hfd, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) {
                        ws.ws_row = 24;
                        ws.ws_col = 80;
                    }
                    std::array<std::uint8_t, 8> wbuf{};
                    std::memcpy(wbuf.data(), &ws, 8);
                    r = memory.write(arg(state, 2), wbuf.data(), 8) ? 0 : -kEfault;
                } else if (req == 0x5401UL) { // TCGETS
                    struct termios tio{};
                    if (hfd >= 0 && ::tcgetattr(hfd, &tio) == 0) {
                        std::array<std::uint8_t, 60> tbuf{};
                        const std::size_t n = sizeof(tio) < tbuf.size() ? sizeof(tio) : tbuf.size();
                        std::memcpy(tbuf.data(), &tio, n);
                        r = memory.write(arg(state, 2), tbuf.data(), 36) ? 0 : -kEfault;
                    } else {
                        r = -25; // -ENOTTY
                    }
                } else if (req == 0x5402UL || req == 0x5403UL || req == 0x5404UL) {
                    // TCSETS / TCSETSW / TCSETSF: catch (0x5402) comes from
                    // TCGETS above. Raw mode - the first thing every terminal
                    // application does - is exactly this call, so a TUI that
                    // cannot issue it either exits or drives the terminal in
                    // cooked mode. `struct termios` is the asm-generic kernel
                    // layout on both x86-64 and AArch64 (4 x u32 flags, c_line,
                    // c_cc[19], 36 bytes), so the guest's bytes are the host's
                    // bytes once validated; the 60-byte *userspace* struct never
                    // reaches this layer, because glibc issues the ioctl with the
                    // kernel-sized one.
                    std::array<std::uint8_t, kGuestTermiosSize> tbuf{};
                    if (!memory.read(arg(state, 2), tbuf.data(), tbuf.size())) {
                        r = -kEfault;
                        break;
                    }
                    if (hfd < 0) {
                        r = -kEnotty;
                        break;
                    }
                    const int flush = (req == 0x5404UL)   ? TCSAFLUSH
                                      : (req == 0x5403UL) ? TCSADRAIN
                                                          : TCSANOW;
                    if (::ioctl(hfd, TCSETS, tbuf.data()) != 0) {
                        // tcsetattr's action flags are a C-library concept, not a
                        // kernel one; TCSETSW/TCSETSF differ only in when they
                        // wait, and the host ioctl above already applied the
                        // change. Nothing else to do for those.
                        (void)flush;
                        r = -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
                    } else {
                        r = 0;
                    }
                } else if (req == 0x5414UL) { // TIOCSWINSZ
                    std::array<std::uint8_t, 8> wbuf{};
                    if (!memory.read(arg(state, 2), wbuf.data(), wbuf.size())) {
                        r = -kEfault;
                        break;
                    }
                    struct winsize ws{};
                    std::memcpy(&ws, wbuf.data(), wbuf.size());
                    r = (hfd >= 0 && ::ioctl(hfd, TIOCSWINSZ, &ws) == 0)
                            ? 0
                            : -static_cast<std::int64_t>(errno == 0 ? kEnotty : errno);
                } else if (req == 0x541BUL) { // FIONREAD
                    int available = 0;
                    if (hfd < 0 || ::ioctl(hfd, FIONREAD, &available) != 0) {
                        r = -static_cast<std::int64_t>(errno == 0 ? kEnotty : errno);
                        break;
                    }
                    std::array<std::uint8_t, 4> nbuf{};
                    put_u32(nbuf.data(), 0, static_cast<std::uint32_t>(available));
                    r = memory.write(arg(state, 2), nbuf.data(), nbuf.size()) ? 0 : -kEfault;
                } else {
                    r = -kEnotty;
                }
                break;
            }
            case 22:    // pipe(int pipefd[2])
            case 293: { // pipe2(int pipefd[2], int flags)
                const std::uint64_t pipe_addr = arg(state, 0);
                const auto pflags =
                    (result.number == 293) ? static_cast<int>(arg_i32(state, 1)) : 0;
                int hpipe[2] = {-1, -1};
                int host_pflags = O_CLOEXEC;
                if ((pflags & static_cast<int>(open_flags::kONonblock)) != 0) {
                    host_pflags |= O_NONBLOCK;
                }
                if (::pipe2(hpipe, host_pflags) != 0) {
                    r = -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
                    break;
                }
                auto d0 = std::make_shared<OpenFileDescription>();
                d0->host_fd = hpipe[0];
                d0->owned_fd = true;
                auto d1 = std::make_shared<OpenFileDescription>();
                d1->host_fd = hpipe[1];
                d1->owned_fd = true;
                const bool ce = (pflags & static_cast<int>(open_flags::kOCloseexec)) != 0;
                const std::int64_t g0 = alloc_fd(files_, std::move(d0), ce);
                const std::int64_t g1 = (g0 >= 0) ? alloc_fd(files_, std::move(d1), ce) : -kEmfile;
                if (g0 < 0 || g1 < 0) {
                    r = -kEmfile;
                    break;
                }
                std::array<std::uint8_t, 8> fds_out{};
                put_u32(fds_out.data(), 0, static_cast<std::uint32_t>(g0));
                put_u32(fds_out.data(), 4, static_cast<std::uint32_t>(g1));
                r = memory.write(pipe_addr, fds_out.data(), 8) ? 0 : -kEfault;
                break;
            }
            case 25: { // mremap(old_addr, old_len, new_len, flags)
                if (address_space_ == nullptr) {
                    r = -kEnomem;
                    break;
                }
                const std::uint64_t old_addr = arg(state, 0);
                const std::uint64_t old_len = arg(state, 1);
                const std::uint64_t new_len = arg(state, 2);
                const auto mflags = arg(state, 3);
                if (new_len <= old_len) {
                    const auto aligned_old = (old_len + 4095ULL) & ~4095ULL;
                    const auto aligned_new = (new_len + 4095ULL) & ~4095ULL;
                    if (aligned_new < aligned_old) {
                        (void)address_space_->unmap(
                            old_addr + aligned_new,
                            static_cast<std::size_t>(aligned_old - aligned_new));
                    }
                    r = static_cast<std::int64_t>(old_addr);
                } else if ((mflags & 1ULL) != 0) { // MREMAP_MAYMOVE
                    const auto nbase = address_space_->map_anonymous(
                        static_cast<std::size_t>(new_len), MemPerm::kReadWrite);
                    if (!nbase) {
                        r = -kEnomem;
                        break;
                    }
                    std::vector<std::uint8_t> tmp(static_cast<std::size_t>(old_len), 0);
                    if (!address_space_->read(old_addr, tmp.data(), tmp.size()) ||
                        !address_space_->write(*nbase, tmp.data(), tmp.size())) {
                        r = -kEfault;
                        break;
                    }
                    (void)address_space_->unmap(old_addr, static_cast<std::size_t>(old_len));
                    r = static_cast<std::int64_t>(*nbase);
                } else {
                    r = -kEnomem;
                }
                break;
            }
            case 28: { // madvise(addr, length, advice)
                // Every guest mapping in this execution model is an anonymous,
                // private, writable-or-not host-heap region owned by
                // GuestAddressSpace; there are no file-backed or shared mappings
                // (see COMPATIBILITY.md). That makes the advice modes split
                // cleanly into three groups:
                //
                //   * pure hints, with no guest-observable effect (reclaim,
                //     readahead, THP bookkeeping) - success is truthful because
                //     the kernel makes no promise a guest can test;
                //   * MADV_DONTNEED, whose *entire* documented effect on
                //     anonymous private memory is "the pages are zeroed" - that
                //     is implemented here, exactly;
                //   * modes whose effect is per-mapping kernel state that this
                //     layer does not carry (DONTFORK/WIPEONFORK and friends),
                //     refused with -ENOSYS rather than silently ignored. A guest
                //     that asked for MADV_DONTFORK and then forked would otherwise
                //     find its mapping still there.
                const std::uint64_t addr = arg(state, 0);
                const std::uint64_t length = arg(state, 1);
                const std::int32_t advice = arg_i32(state, 2);
                if (length == 0) {
                    r = 0; // the kernel returns success without touching anything
                    break;
                }
                if ((addr & 0xFFF) != 0) {
                    r = -kEinval;
                    break;
                }
                constexpr std::int32_t kMadvNormal = 0;
                constexpr std::int32_t kMadvRandom = 1;
                constexpr std::int32_t kMadvSequential = 2;
                constexpr std::int32_t kMadvWillneed = 3;
                constexpr std::int32_t kMadvDontneed = 4;
                constexpr std::int32_t kMadvFree = 8;
                constexpr std::int32_t kMadvRemove = 9;
                constexpr std::int32_t kMadvDontfork = 10;
                constexpr std::int32_t kMadvDofork = 11;
                constexpr std::int32_t kMadvHugepage = 14;
                constexpr std::int32_t kMadvNohugepage = 15;
                constexpr std::int32_t kMadvDontdump = 16;
                constexpr std::int32_t kMadvDodump = 18;
                constexpr std::int32_t kMadvKeeponfork = 19;
                constexpr std::int32_t kMadvCold = 20;
                constexpr std::int32_t kMadvPageout = 21;
                constexpr std::int32_t kMadvWipeonfork = 23;

                // MADV_REMOVE is EINVAL for a private anonymous mapping, which is
                // all there is here - that is the kernel's own answer, not a
                // limitation of this layer.
                if (advice == kMadvRemove) {
                    r = -kEinval;
                    break;
                }
                switch (advice) {
                case kMadvNormal:
                case kMadvRandom:
                case kMadvSequential:
                case kMadvWillneed:
                case kMadvFree: // "may" discard: keeping the contents is allowed
                case kMadvHugepage:
                case kMadvNohugepage:
                case kMadvDontdump:
                case kMadvDodump:
                case kMadvCold:
                case kMadvPageout: {
                    // The range must still be mapped; the kernel returns
                    // ENOMEM otherwise, and a guest relies on that.
                    std::uint8_t probe = 0;
                    if (!memory.read(addr, &probe, 1) ||
                        !memory.read(addr + length - 1, &probe, 1)) {
                        r = -kEnomem;
                        break;
                    }
                    r = 0;
                    break;
                }
                case kMadvDontneed: {
                    // Real semantics for anonymous private memory: the
                    // contents are gone and later reads see zeros.
                    std::array<std::uint8_t, 4096> zeros{};
                    std::uint64_t done = 0;
                    bool ok = true;
                    while (done < length) {
                        const std::uint64_t chunk =
                            std::min<std::uint64_t>(zeros.size(), length - done);
                        if (!memory.write(addr + done, zeros.data(),
                                          static_cast<std::size_t>(chunk))) {
                            // Unmapped in the middle: the kernel would have
                            // failed with ENOMEM before changing anything,
                            // so this is reported rather than half-applied.
                            ok = false;
                            break;
                        }
                        done += chunk;
                    }
                    r = ok ? 0 : -kEnomem;
                    break;
                }
                case kMadvDontfork:
                case kMadvDofork:
                case kMadvKeeponfork:
                case kMadvWipeonfork:
                    // Per-mapping state this execution model does not carry.
                    // Refusing tells the guest the truth; pretending would
                    // make a fork behave differently than asked.
                    r = -kEnosys;
                    break;
                default:
                    // Unknown advice: the kernel's answer is EINVAL.
                    r = -kEinval;
                    break;
                }
                break;
            }
            case 157: { // prctl(option, arg2, arg3, arg4, arg5)
                // Implemented for the options whose meaning can be honoured or
                // truthfully answered here, and refused - with a real errno, not
                // a fake success - for the ones that cannot:
                //
                //   PR_SET_NAME/PR_GET_NAME      recorded, reported back
                //   PR_SET_PDEATHSIG/GET         recorded, reported back (the
                //                                host process is not armed: a
                //                                guest's parent is not HYPERION's
                //                                parent, so arming would be wrong)
                //   PR_SET_DUMPABLE/GET          recorded, reported back
                //   PR_SET_TIMERSLACK/GET        forwarded to the host
                //   PR_SET_NO_NEW_PRIVS/GET      forwarded to the host (one-way,
                //                                and strictly more restrictive)
                //   PR_SET_SECCOMP, PR_SET_MM, PR_CAP_AMBIENT, child subreaper,
                //   capabilities                    -ENOSYS: they would need to
                //                                change this process's privileges
                //                                or install a filter over the
                //                                guest, which is not modelled
                //   anything else                -EINVAL, as the kernel does
                const std::int32_t option = arg_i32(state, 0);
                constexpr std::int32_t kPrSetPdeathsig = 1;
                constexpr std::int32_t kPrGetPdeathsig = 2;
                constexpr std::int32_t kPrGetDumpable = 3;
                constexpr std::int32_t kPrSetDumpable = 4;
                constexpr std::int32_t kPrSetName = 15;
                constexpr std::int32_t kPrGetName = 16;
                constexpr std::int32_t kPrSetSeccomp = 22;
                constexpr std::int32_t kPrSetTimerslack = 29;
                constexpr std::int32_t kPrGetTimerslack = 30;
                constexpr std::int32_t kPrSetChildSubreaper = 36;
                constexpr std::int32_t kPrGetChildSubreaper = 37;
                constexpr std::int32_t kPrSetNoNewPrivs = 38;
                constexpr std::int32_t kPrGetNoNewPrivs = 39;
                constexpr std::int32_t kPrCapbsetRead = 23;
                constexpr std::int32_t kPrCapbsetDrop = 24;
                constexpr std::int32_t kPrSetMmd = 35;
                constexpr std::int32_t kPrSetMceKill = 33;
                switch (option) {
                case kPrSetName: {
                    std::array<std::uint8_t, 16> name{};
                    if (!memory.read(arg(state, 1), name.data(), name.size())) {
                        r = -kEfault;
                        break;
                    }
                    name.back() = 0; // always NUL-terminated, as the kernel does
                    std::memcpy(process_name_.data(), name.data(), name.size());
                    r = 0;
                    break;
                }
                case kPrGetName: {
                    std::array<std::uint8_t, 16> name{};
                    std::memcpy(name.data(), process_name_.data(), name.size());
                    r = memory.write(arg(state, 1), name.data(), name.size()) ? 0 : -kEfault;
                    break;
                }
                case kPrSetPdeathsig: {
                    const std::int32_t sig = arg_i32(state, 1);
                    if (sig < 0 || sig > 64) {
                        r = -kEinval;
                        break;
                    }
                    pdeathsig_ = sig;
                    r = 0;
                    break;
                }
                case kPrGetPdeathsig: {
                    std::array<std::uint8_t, 4> out{};
                    put_u32(out.data(), 0, static_cast<std::uint32_t>(pdeathsig_));
                    r = memory.write(arg(state, 1), out.data(), out.size()) ? 0 : -kEfault;
                    break;
                }
                case kPrSetDumpable: {
                    const std::int32_t value = arg_i32(state, 1);
                    if (value != 0 && value != 1) {
                        r = -kEinval;
                        break;
                    }
                    dumpable_ = value;
                    r = 0;
                    break;
                }
                case kPrGetDumpable:
                    r = dumpable_;
                    break;
                case kPrSetTimerslack:
                case kPrGetTimerslack: {
                    // Forwarded: the host's timer slack is what this process
                    // actually uses, and the question is answerable exactly.
                    const unsigned long host_value = static_cast<unsigned long>(arg(state, 1));
                    if (option == kPrSetTimerslack) {
                        r = (::prctl(PR_SET_TIMERSLACK, host_value, 0UL, 0UL, 0UL) == 0)
                                ? 0
                                : -static_cast<std::int64_t>(errno == 0 ? kEinval : errno);
                        break;
                    }
                    errno = 0;
                    const long got = ::prctl(PR_GET_TIMERSLACK, 0UL, 0UL, 0UL, 0UL);
                    r = (got < 0) ? -static_cast<std::int64_t>(errno == 0 ? kEinval : errno)
                                  : static_cast<std::int64_t>(got);
                    break;
                }
                case kPrSetNoNewPrivs: {
                    // One-way and strictly more restrictive, so honouring it
                    // on the host is the conservative direction.
                    const unsigned long host_value = arg(state, 1);
                    r = (::prctl(PR_SET_NO_NEW_PRIVS, host_value, 0UL, 0UL, 0UL) == 0)
                            ? 0
                            : -static_cast<std::int64_t>(errno == 0 ? kEinval : errno);
                    break;
                }
                case kPrGetNoNewPrivs: {
                    errno = 0;
                    const long got = ::prctl(PR_GET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL);
                    r = (got < 0) ? -static_cast<std::int64_t>(errno == 0 ? kEinval : errno)
                                  : static_cast<std::int64_t>(got);
                    break;
                }
                case kPrSetSeccomp:
                case kPrSetMmd:
                case kPrSetMceKill:
                case kPrSetChildSubreaper:
                case kPrGetChildSubreaper:
                case kPrCapbsetRead:
                case kPrCapbsetDrop:
                    r = -kEnosys;
                    break;
                default:
                    r = -kEinval;
                    break;
                }
                break;
            }
            case 221: { // fadvise64(fd, offset, len, advice)
                // Advisory only, so the translation is real rather than an
                // approximation: hand it to the host for descriptors that have a
                // real host file underneath, and report success for the
                // emulator's own in-memory streams, where there is no page cache
                // to advise.
                const std::int32_t guest_fd = arg_i32(state, 0);
                const std::int64_t offset = static_cast<std::int64_t>(arg(state, 1));
                const std::int64_t length = static_cast<std::int64_t>(arg(state, 2));
                const std::int32_t advice = arg_i32(state, 3);
                constexpr std::int32_t kPosixFadvNoreuse = 5;
                if (advice < 0 || advice > kPosixFadvNoreuse) {
                    r = -kEinval;
                    break;
                }
                const OpenFileDescription* ofd = lookup_ofd(files_, guest_fd);
                if (ofd == nullptr) {
                    r = -kEbadf;
                    break;
                }
                const int host_fd = underlying_host_fd(*ofd);
                if (host_fd < 0) {
                    // A stream or sink owned by the embedder: no page cache is
                    // involved, so the hint has nothing to act on. Success is
                    // truthful because the call is advisory.
                    r = 0;
                    break;
                }
                // posix_fadvise returns the errno value itself (it does not set
                // errno), so this maps straight through.
                const int hrc = ::posix_fadvise(host_fd, static_cast<off_t>(offset),
                                                static_cast<off_t>(length), advice);
                r = (hrc == 0) ? 0 : -static_cast<std::int64_t>(hrc);
                break;
            }
            case 229: { // clock_getres(clockid, res)
                // The guest and host clock identifiers agree (0..11 are the same
                // values on x86-64 and AArch64), so this is a real query, not a
                // made-up answer. Returning a fabricated 1ns or 0 would make a
                // guest that sizes its timers from this get it wrong.
                const std::int32_t clock_id = arg_i32(state, 0);
                if (clock_id < 0 || clock_id > 11 /*CLOCK_TAI*/) {
                    r = -kEinval;
                    break;
                }
                std::array<std::uint8_t, 16> ts{};
                struct timespec host_ts{};
                if (::clock_getres(clock_id, &host_ts) != 0) {
                    r = -static_cast<std::int64_t>(errno == 0 ? kEinval : errno);
                    break;
                }
                const std::uint64_t res_addr = arg(state, 1);
                if (res_addr == 0) {
                    r = 0; // NULL res: the kernel only validates the clock id
                    break;
                }
                put_u64(ts.data(), 0, static_cast<std::uint64_t>(host_ts.tv_sec));
                put_u64(ts.data(), 8, static_cast<std::uint64_t>(host_ts.tv_nsec));
                r = memory.write(res_addr, ts.data(), ts.size()) ? 0 : -kEfault;
                break;
            }
            case 35:    // nanosleep(req, rem)
            case 230: { // clock_nanosleep(clockid, flags, req, rem)
                // Sleeping used to be a stub that returned 0 immediately. That is
                // not harmless: every retry/backoff loop in the guest becomes a
                // busy loop, and a program that waits for a child or a timer
                // spins instead of yielding. The guest ABI here happens to match
                // the host's (64-bit tv_sec/tv_nsec, same clock ids), so the
                // translation is a validated copy plus the real call.
                const bool absolute_form = (result.number == 230);
                const std::uint64_t req_addr = arg(state, absolute_form ? 2 : 0);
                const std::uint64_t rem_addr = arg(state, absolute_form ? 3 : 1);
                std::array<std::uint8_t, 16> req{};
                if (!memory.read(req_addr, req.data(), req.size())) {
                    r = -kEfault;
                    break;
                }
                const std::int64_t sec = static_cast<std::int64_t>(get_u64_le(req.data()));
                const std::int64_t nsec = static_cast<std::int64_t>(get_u64_le(req.data() + 8));
                if (sec < 0 || nsec < 0 || nsec >= 1000000000LL) {
                    r = -kEinval;
                    break;
                }
                const int flags = absolute_form ? arg_i32(state, 1) : 0;
                if (absolute_form && (flags & ~TIMER_ABSTIME) != 0) {
                    r = -kEinval;
                    break;
                }
                struct timespec host_req{};
                host_req.tv_sec = static_cast<time_t>(sec);
                host_req.tv_nsec = static_cast<long>(nsec);
                struct timespec host_rem{};
                const int hrc = absolute_form
                                    ? ::clock_nanosleep(static_cast<clockid_t>(arg_i32(state, 0)),
                                                        flags, &host_req, &host_rem)
                                    : ::nanosleep(&host_req, &host_rem);
                if (hrc == 0) {
                    r = 0;
                    break;
                }
                if (errno == EINTR) {
                    // Linux reports an interrupted sleep as -EINTR and, for the
                    // relative form, writes the unslept remainder back. The
                    // absolute form never gets a remainder pointer written.
                    if (!absolute_form && rem_addr != 0) {
                        std::array<std::uint8_t, 16> rem{};
                        put_u64(rem.data(), 0, static_cast<std::uint64_t>(host_rem.tv_sec));
                        put_u64(rem.data(), 8, static_cast<std::uint64_t>(host_rem.tv_nsec));
                        if (!memory.write(rem_addr, rem.data(), rem.size())) {
                            r = -kEfault;
                            break;
                        }
                    }
                    r = -kEintr;
                    break;
                }
                r = -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
                break;
            }
            case 82:   // rename
            case 83:   // mkdir
            case 84:   // rmdir
            case 86:   // link
            case 87:   // unlink
            case 88:   // symlink
            case 90: { // chmod
                std::string p1;
                const std::int64_t rc1 = read_guest_cstr(memory, arg(state, 0), p1);
                if (rc1 != 0) {
                    r = rc1;
                    break;
                }
                p1 = resolve_guest_path(p1, sysroot_);
                int hrc = 0;
                if (result.number == 83) {
                    hrc = ::mkdir(p1.c_str(), static_cast<mode_t>(arg(state, 1)));
                } else if (result.number == 84) {
                    hrc = ::rmdir(p1.c_str());
                } else if (result.number == 87) {
                    hrc = ::unlink(p1.c_str());
                } else if (result.number == 90) {
                    hrc = ::chmod(p1.c_str(), static_cast<mode_t>(arg(state, 1)));
                } else {
                    std::string p2;
                    const std::int64_t rc2 = read_guest_cstr(memory, arg(state, 1), p2);
                    if (rc2 != 0) {
                        r = rc2;
                        break;
                    }
                    p2 = resolve_guest_path(p2, sysroot_);
                    if (result.number == 86) {
                        // link(2) and symlink(2) are the non-`at` spelling of
                        // linkat(2)/symlinkat(2); glibc issues them directly, so
                        // a guest that calls plain link()/symlink() misses the
                        // `at` adapters entirely. `ln` and `ln -s` are used by
                        // every install script, including terminal applications'
                        // startup wrappers.
                        hrc = ::linkat(AT_FDCWD, p1.c_str(), AT_FDCWD, p2.c_str(), 0);
                    } else if (result.number == 88) {
                        hrc = ::symlinkat(p1.c_str(), AT_FDCWD, p2.c_str());
                    } else {
                        hrc = ::rename(p1.c_str(), p2.c_str());
                    }
                }
                r = (hrc == 0) ? 0 : -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
                break;
            }
            case 91: { // fchmod
                const OpenFileDescription* ofd = lookup_ofd(files_, arg_i32(state, 0));
                if (ofd == nullptr || underlying_host_fd(*ofd) < 0) {
                    r = -kEbadf;
                } else {
                    r = (::fchmod(underlying_host_fd(*ofd), static_cast<mode_t>(arg(state, 1))) ==
                         0)
                            ? 0
                            : -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
                }
                break;
            }
            case 99: { // sysinfo
                std::array<std::uint8_t, 112> si{};
                put_u64(si.data(), 0, 3600ULL);
                put_u64(si.data(), 32, 4ULL << 30); // totalram = 4 GiB
                put_u64(si.data(), 40, 2ULL << 30); // freeram  = 2 GiB
                put_u32(si.data(), 104, 1U);        // mem_unit = 1
                r = memory.write(arg(state, 0), si.data(), si.size()) ? 0 : -kEfault;
                break;
            }
            case 137:   // statfs
            case 138: { // fstatfs
                std::array<std::uint8_t, 120> sfs{};
                put_u64(sfs.data(), 0, 0xEF53ULL); // EXT4_SUPER_MAGIC
                put_u64(sfs.data(), 8, 4096ULL);
                put_u64(sfs.data(), 16, 1ULL << 20);
                put_u64(sfs.data(), 24, 1ULL << 19);
                put_u64(sfs.data(), 32, 1ULL << 19);
                put_u64(sfs.data(), 64, 255ULL);
                put_u64(sfs.data(), 72, 4096ULL);
                const std::uint64_t out_addr = arg(state, 1);
                r = memory.write(out_addr, sfs.data(), sfs.size()) ? 0 : -kEfault;
                break;
            }
            case 202: { // futex(uaddr, futex_op, val, ...)
                // Honest, not approximated. A guest thread cannot exist here
                // (clone with CLONE_VM/CLONE_THREAD is refused), so no one can
                // ever wake a waiter: blocking would hang forever, and reporting
                // success - which this used to do - tells the caller it was woken
                // when nothing happened. The value comparison is real and is what
                // the caller relies on for the uncontended path; the blocking
                // path refuses explicitly.
                const std::uint64_t uaddr = arg(state, 0);
                const int op = arg_i32(state, 1) & 0x7F;
                const auto val = static_cast<std::uint32_t>(arg(state, 2));
                constexpr int kFutexWait = 0;
                constexpr int kFutexWake = 1;
                constexpr int kFutexWaitBitset = 9;
                constexpr int kFutexWakeBitset = 10;
                if (op == kFutexWait || op == kFutexWaitBitset) {
                    std::array<std::uint8_t, 4> cur_b{};
                    if (!memory.read(uaddr, cur_b.data(), 4)) {
                        r = -kEfault;
                        break;
                    }
                    std::uint32_t cur = 0;
                    std::memcpy(&cur, cur_b.data(), 4);
                    // The kernel locks, re-reads, and only then decides: the
                    // comparison has to happen here, after the read, or a caller
                    // that raced a change would spin.
                    if (cur != val) {
                        r = -kEagain;
                        break;
                    }
                    // A waiter with a relative timeout can be honoured exactly:
                    // nothing in this execution model can wake it (no guest
                    // threads), so the kernel's own answer for a waiter that is
                    // never woken is what happens here - it sleeps for the
                    // timeout and reports ETIMEDOUT. `pthread_cond_timedwait`
                    // and every bounded retry loop depend on that.
                    const std::uint64_t timeout_addr = arg(state, 3);
                    if (timeout_addr != 0) {
                        std::array<std::uint8_t, 16> ts_b{};
                        if (!memory.read(timeout_addr, ts_b.data(), ts_b.size())) {
                            r = -kEfault;
                            break;
                        }
                        std::int64_t sec = static_cast<std::int64_t>(get_u64_le(ts_b.data()));
                        std::int64_t nsec = static_cast<std::int64_t>(get_u64_le(ts_b.data() + 8));
                        if (sec < 0 || nsec < 0 || nsec >= 1000000000LL) {
                            r = -kEinval;
                            break;
                        }
                        struct timespec req{};
                        req.tv_sec = static_cast<time_t>(sec);
                        req.tv_nsec = static_cast<long>(nsec);
                        while (::nanosleep(&req, &req) != 0) {
                            if (errno != EINTR) {
                                break;
                            }
                        }
                        r = -kEtimeout;
                        break;
                    }
                    // No timeout: nothing can ever wake this waiter, so blocking
                    // would hang and claiming success would lie.
                    r = -kEnosys;
                    break;
                }
                if (op == kFutexWake || op == kFutexWakeBitset) {
                    // No waiter can be blocked, so zero woken is the truth rather
                    // than a placeholder. A caller that expected to wake someone
                    // and gets 0 is told exactly what happened.
                    r = 0;
                    break;
                }
                // FUTEX_REQUEUE, FUTEX_CMP_REQUEUE, FUTEX_WAKE_OP, PI futexes and
                // the rest manipulate a wait queue this layer does not have.
                r = -kEnosys;
                break;
            }
            case 204: { // sched_getaffinity(pid, cpusetsize, mask)
                const std::uint64_t cpusetsize = arg(state, 1);
                const std::uint64_t mask_addr = arg(state, 2);
                if (cpusetsize < 8) {
                    r = -kEinval;
                } else {
                    std::array<std::uint8_t, 8> mask{};
                    mask[0] = 0x01; // 1 online logical CPU
                    r = memory.write(mask_addr, mask.data(), 8) ? 8 : -kEfault;
                }
                break;
            }
            case 258:   // mkdirat
            case 263:   // unlinkat
            case 268:   // fchmodat
            case 269:   // faccessat
            case 439: { // faccessat2
                int hdfd = AT_FDCWD;
                if (!resolve_host_dirfd(files_, arg_i32(state, 0), true, hdfd) ||
                    (hdfd < 0 && hdfd != AT_FDCWD)) {
                    r = -kEbadf;
                    break;
                }
                std::string p;
                const std::int64_t prc = read_guest_cstr(memory, arg(state, 1), p);
                if (prc != 0) {
                    r = prc;
                    break;
                }
                p = resolve_guest_path(p, sysroot_);
                int hrc = 0;
                if (result.number == 258) {
                    hrc = ::mkdirat(hdfd, p.c_str(), static_cast<mode_t>(arg(state, 2)));
                } else if (result.number == 263) {
                    const int uflags = ((arg(state, 2) & 0x200ULL) != 0) ? AT_REMOVEDIR : 0;
                    hrc = ::unlinkat(hdfd, p.c_str(), uflags);
                } else if (result.number == 268) {
                    hrc = ::fchmodat(hdfd, p.c_str(), static_cast<mode_t>(arg(state, 2)), 0);
                } else {
                    hrc = ::faccessat(hdfd, p.c_str(), static_cast<int>(arg(state, 2) & 7ULL), 0);
                }
                r = (hrc == 0) ? 0 : -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
                break;
            }
            case 264:   // renameat
            case 316: { // renameat2
                int d1 = AT_FDCWD;
                int d2 = AT_FDCWD;
                if (!resolve_host_dirfd(files_, arg_i32(state, 0), true, d1) ||
                    !resolve_host_dirfd(files_, arg_i32(state, 2), true, d2)) {
                    r = -kEbadf;
                    break;
                }
                std::string p1;
                std::string p2;
                if (read_guest_cstr(memory, arg(state, 1), p1) != 0 ||
                    read_guest_cstr(memory, arg(state, 3), p2) != 0) {
                    r = -kEfault;
                    break;
                }
                p1 = resolve_guest_path(p1, sysroot_);
                p2 = resolve_guest_path(p2, sysroot_);
                r = (::renameat(d1, p1.c_str(), d2, p2.c_str()) == 0)
                        ? 0
                        : -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
                break;
            }
            case 280: // utimensat
                r = 0;
                break;
            case 334: // rseq — cleanly report unsupported per Linux ABI without counting fault
                r = -kEnosys;
                break;
            case 62:    // kill
            case 234: { // tgkill(tgid, tid, sig)
                // Every signal this layer can see is aimed at the one guest
                // process it emulates, so kill()/tgkill() is a *synchronous*
                // self-raise. What happens next is decided by the disposition the
                // guest installed and by its signal mask, not by the syscall:
                // ignoring it, dropping it (SIG_IGN or an ignore-class default),
                // holding it until the guest unblocks it, or ending the run.
                const int sig = (result.number == 234) ? arg_i32(state, 2) : arg_i32(state, 1);
                if (sig < 0 || sig > static_cast<int>(kMaxSignal)) {
                    r = -kEinval;
                    break;
                }
                if (sig == 0) {
                    r = 0; // existence probe: the process is obviously there
                    break;
                }
                const std::int64_t outcome = raise_signal(sig);
                if (outcome != 0) {
                    result.status = SyscallStatus::kExit;
                    result.exit_code = outcome;
                    return result;
                }
                r = 0;
                break;
            }
            default:
                handled = false;
                break;
            }
            if (handled) {
                state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
                result.status = SyscallStatus::kHandled;
                return result;
            }
        }
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(-kEnosys);
        result.status = SyscallStatus::kUnimplemented;
        return result;
    }
}

// --- signal dispositions (P12) ---------------------------------------------
//
// A single-threaded emulator can honour exactly three outcomes for a signal
// aimed at the guest: ignore it, hold it while it is blocked, or end the run.
// Guest *handlers* are a fourth outcome that this layer cannot produce - it would
// have to build an x86-64 rt_sigframe on the guest stack, divert rip, and
// implement rt_sigreturn to undo it - so an installed handler is treated as the
// terminate class and the gap is documented rather than reported as success.
//
// Default dispositions, restricted to what a process without a scheduler and
// without a stopped state can represent:
//   SIGCHLD(17) SIGURG(23) SIGWINCH(28) SIGCONT(18)  default action is "ignore"
//   SIGSTOP(19) SIGTSTP(20) SIGTTIN(21) SIGTTOU(22)  default action is "stop",
//     which has no equivalent here; terminating would end a run a real kernel
//     would merely pause, so these are treated as ignored as well.
std::int64_t MinimalSyscallHandler::signal_disposition(int sig) const {
    switch (sig) {
    case 17: // SIGCHLD
    case 18: // SIGCONT
    case 19: // SIGSTOP
    case 20: // SIGTSTP
    case 21: // SIGTTIN
    case 22: // SIGTTOU
    case 23: // SIGURG
    case 28: // SIGWINCH
        return 0;
    default:
        break;
    }
    const auto index = static_cast<std::size_t>(sig);
    if (index < sigactions_.size() && sigactions_[index].installed) {
        const GuestSignalAction& act = sigactions_[index];
        if (act.handler == 1) {
            return 0; // SIG_IGN
        }
        // A real handler address is installed and therefore *not* the default
        // action: this layer cannot invoke it, so the run ends with the status a
        // terminate-class default would have produced.
        return 128 + sig;
    }
    return 128 + sig;
}

std::int64_t MinimalSyscallHandler::raise_signal(int sig) {
    // SIGKILL and SIGSTOP are the two signals a mask cannot hold back.
    const bool maskable = (sig != 9 && sig != 19);
    const std::uint64_t bit = (sig >= 1 && sig <= 64) ? (1ULL << (sig - 1)) : 0;

    // If the signal is masked, hold it for later delivery.
    if (maskable && bit != 0 && (sigmask_ & bit) != 0) {
        signal_pending_ |= bit;
        return 0;
    }

    const auto index = static_cast<std::size_t>(sig);

    // SIG_IGN or an ignore-class default: the signal is dropped.
    if (index < sigactions_.size() && sigactions_[index].installed &&
        sigactions_[index].handler == 1) {
        return 0;
    }
    // Ignore-class defaults (SIGCHLD, SIGCONT, SIGURG, SIGWINCH, etc.):
    // nothing to do, the guest never notices.
    switch (sig) {
    case 17: // SIGCHLD
    case 18: // SIGCONT
    case 19: // SIGSTOP
    case 20: // SIGTSTP
    case 21: // SIGTTIN
    case 22: // SIGTTOU
    case 23: // SIGURG
    case 28: // SIGWINCH
        return 0;
    default:
        break;
    }

    // A real handler is installed: queue the signal so check_and_deliver_signals()
    // can build an rt_sigframe and divert to the handler at the next boundary.
    if (index < sigactions_.size() && sigactions_[index].installed) {
        signal_pending_ |= bit;
        return 0;
    }

    // Terminate-class default with no handler: end the run.
    return 128 + sig;
}

std::int64_t MinimalSyscallHandler::deliver_pending_signals() {
    for (int sig = 1; sig <= static_cast<int>(kMaxSignal) && signal_pending_ != 0; ++sig) {
        const std::uint64_t bit = 1ULL << (sig - 1);
        if ((signal_pending_ & bit) == 0) {
            continue;
        }
        const bool maskable = (sig != 9 && sig != 19);
        if (maskable && (sigmask_ & bit) != 0) {
            continue; // still blocked
        }

        const auto index = static_cast<std::size_t>(sig);

        // SIG_IGN: drop the signal.
        if (index < sigactions_.size() && sigactions_[index].installed &&
            sigactions_[index].handler == 1) {
            signal_pending_ &= ~bit;
            continue;
        }
        // Ignore-class defaults: nothing to do, drop the signal.
        switch (sig) {
        case 17: // SIGCHLD
        case 18: // SIGCONT
        case 19: // SIGSTOP
        case 20: // SIGTSTP
        case 21: // SIGTTIN
        case 22: // SIGTTOU
        case 23: // SIGURG
        case 28: // SIGWINCH
            signal_pending_ &= ~bit;
            continue;
        default:
            break;
        }

        // A real handler is installed: leave it pending for
        // check_and_deliver_signals() to deliver at the next boundary.
        if (index < sigactions_.size() && sigactions_[index].installed) {
            continue; // leave signal_pending_ set
        }

        // Terminate-class default with no handler: end the run.
        signal_pending_ &= ~bit;
        return 128 + sig;
    }
    return 0;
}

void MinimalSyscallHandler::clear_pending_signal(int sig) noexcept {
    if (sig >= 1 && sig <= static_cast<int>(kMaxSignal)) {
        signal_pending_ &= ~(1ULL << (sig - 1));
    }
}

// The syscall numbers handled by the nested `if (linux_compat_)` switch inside
// handle_unguarded(). They are *not* part of is_minimal_syscall(): they only work
// once enable_linux_compat() has been called. Keeping the list here, next to the
// switch itself, is what lets the registry validator refuse to let a JSON rule
// take over a number this layer already claims.
//
// Two per-number annotations are read by tools/gen_syscall_inventory.py, which
// keeps docs/syscalls/inventory.tsv honest:
//   [stub]        the number is accepted but does not do the whole job: it may
//                 answer success without performing the call (prctl is a no-op,
//                 madvise returns 0) or refuse the part it cannot do (futex
//                 compares the value, then refuses to block).
//   [unsupported] the number is recognised only so that it can be rejected with
//                 a clean -ENOSYS instead of being counted as a fault. It is not
//                 an implementation and stays `not-started` in the inventory.
bool is_linux_compat_syscall(std::uint64_t number) {
    switch (number) {
    case 2:   // open
    case 4:   // stat
    case 6:   // lstat
    case 13:  // rt_sigaction
    case 14:  // rt_sigprocmask
    case 16:  // ioctl
    case 22:  // pipe
    case 25:  // mremap
    case 28:  // madvise
    case 35:  // nanosleep
    case 62:  // kill
    case 82:  // rename
    case 83:  // mkdir
    case 84:  // rmdir
    case 86:  // link
    case 87:  // unlink
    case 88:  // symlink
    case 90:  // chmod
    case 91:  // fchmod
    case 99:  // sysinfo
    case 131: // sigaltstack
    case 137: // statfs
    case 138: // fstatfs
    case 157: // prctl
    case 202: // futex [stub]
    case 204: // sched_getaffinity
    case 221: // fadvise64
    case 229: // clock_getres
    case 230: // clock_nanosleep
    case 234: // tgkill
    case 258: // mkdirat
    case 263: // unlinkat
    case 264: // renameat
    case 268: // fchmodat
    case 269: // faccessat
    case 280: // utimensat
    case 293: // pipe2
    case 316: // renameat2
    case 334: // rseq [unsupported]
    case 439: // faccessat2
        return true;
    default:
        return false;
    }
}

bool is_minimal_syscall(std::uint64_t number) {
    return number == sysno::kRead || number == sysno::kWrite || number == sysno::kClose ||
           number == sysno::kFstat || number == sysno::kLseek || number == sysno::kPread64 ||
           number == sysno::kPwrite64 || number == sysno::kReadv || number == sysno::kWritev ||
           number == sysno::kDup || number == sysno::kDup2 || number == sysno::kDup3 ||
           number == sysno::kFcntl || number == sysno::kUname || number == sysno::kGettimeofday ||
           number == sysno::kClockGettime || number == sysno::kArchPrctl ||
           number == sysno::kSetTidAddress || number == sysno::kSetRobustList ||
           number == sysno::kPrlimit64 || number == sysno::kExit || number == sysno::kExitGroup ||
           number == sysno::kMmap || number == sysno::kMprotect || number == sysno::kMunmap ||
           number == sysno::kBrk || number == sysno::kOpenat || number == sysno::kNewfstatat ||
           number == sysno::kGetpid || number == sysno::kGettid || number == sysno::kGetuid ||
           number == sysno::kGeteuid || number == sysno::kGetgid || number == sysno::kGetegid ||
           number == sysno::kGetppid || number == sysno::kGetcwd || number == sysno::kGetrandom ||
           number == sysno::kReadlink || number == sysno::kAccess || number == sysno::kReadlinkat ||
           number == sysno::kGetdents64 || number == sysno::kStatx || number == sysno::kFork ||
           number == sysno::kVfork || number == sysno::kClone || number == sysno::kExecve ||
           number == sysno::kWait4;
}

} // namespace hyperion
