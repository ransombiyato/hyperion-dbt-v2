// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest Linux system-call layer (Phase 6, increment 1).
//
// The x86-64 `syscall` instruction (0F 05) transfers control to the kernel with
// the number in rax and arguments in rdi, rsi, rdx, r10, r8, r9. Hyperion runs
// guest code as an ordinary unprivileged host process, so there is no real
// kernel transition: the instruction is routed to a SyscallHandler that
// emulates the call in userspace and writes the result back into rax.
//
// Scope of this increment is deliberately tiny and stated rather than implied:
//   write(2), read(2), close(2), lseek(2), openat(2), fstat(2), exit(2),
//   exit_group(2), the memory syscalls mmap(2), mprotect(2), munmap(2), brk(2),
//   the process-identity getters getpid(2), gettid(2), getuid(2), geteuid(2),
//   getgid(2), getegid(2), getppid(2), getcwd(2), and getrandom(2), the
//   path-metadata call newfstatat(2), and the path queries access(2) and
//   readlink(2).
//
// Process creation (fork(2), vfork(2), the process-creating subset of clone(2),
// execve(2) and wait4(2)) is implemented in src/core/process.cpp and documented
// in COMPATIBILITY.md. It is a real subset, not a complete implementation:
// thread-creating clone flags are rejected with -ENOSYS rather than aliased to
// fork, because a guest that asked for CLONE_VM would corrupt its own memory if
// it silently received a private copy.
//
// Every other syscall number returns -ENOSYS explicitly. Nothing silently
// succeeds. openat() is limited to AT_FDCWD and a guest pathname; fstat() and
// newfstatat() write the *guest's* (x86-64) struct stat layout, not the host's.
// The identity getters forward to the host process (Hyperion runs the guest
// in-process, so the guest sees the host's ids). The full inventory and the
// implementation order live in docs/syscalls/.
//
// Isolation: this is not a sandbox. openat() forwards to the host kernel, so the
// guest can open whatever the *host process* may open, exactly as SECURITY.md
// states. Run untrusted guests only inside an external isolation boundary.
//
// Error convention: a failed syscall returns a *negative* errno in rax (e.g.
// -EFAULT), exactly as the Linux kernel does; it does not return -1 and set a
// host errno. This is what guest programs test for.
#ifndef HYPERION_SYSCALL_HPP
#define HYPERION_SYSCALL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include "hyperion/address_space.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp" // Memory

namespace hyperion {

// x86-64 syscall numbers used by this increment. The authoritative list is
// docs/syscalls/kernel-table-x86_64-6.12.tsv; these are repeated here so the
// handler does not depend on the documentation.
namespace sysno {
inline constexpr std::uint64_t kRead = 0;
inline constexpr std::uint64_t kWrite = 1;
inline constexpr std::uint64_t kRtSigaction = 13;
inline constexpr std::uint64_t kRtSigprocmask = 14;
inline constexpr std::uint64_t kRtSigreturn = 15;
inline constexpr std::uint64_t kClose = 3;
inline constexpr std::uint64_t kFstat = 5;
inline constexpr std::uint64_t kLseek = 8;
inline constexpr std::uint64_t kMmap = 9;
inline constexpr std::uint64_t kMprotect = 10;
inline constexpr std::uint64_t kMunmap = 11;
inline constexpr std::uint64_t kBrk = 12;
inline constexpr std::uint64_t kPread64 = 17;
inline constexpr std::uint64_t kPwrite64 = 18;
inline constexpr std::uint64_t kReadv = 19;
inline constexpr std::uint64_t kWritev = 20;
inline constexpr std::uint64_t kAccess = 21;
inline constexpr std::uint64_t kDup = 32;
inline constexpr std::uint64_t kDup2 = 33;
inline constexpr std::uint64_t kGetpid = 39;
inline constexpr std::uint64_t kFork = 57;
inline constexpr std::uint64_t kVfork = 58;
inline constexpr std::uint64_t kClone = 56;
inline constexpr std::uint64_t kExit = 60;
inline constexpr std::uint64_t kKill = 62;
inline constexpr std::uint64_t kWait4 = 61;
inline constexpr std::uint64_t kExecve = 59;
inline constexpr std::uint64_t kUname = 63;
inline constexpr std::uint64_t kFcntl = 72;
inline constexpr std::uint64_t kGetcwd = 79;
inline constexpr std::uint64_t kReadlink = 89;
inline constexpr std::uint64_t kGettimeofday = 96;
inline constexpr std::uint64_t kGetuid = 102;
inline constexpr std::uint64_t kGetgid = 104;
inline constexpr std::uint64_t kGeteuid = 107;
inline constexpr std::uint64_t kGetegid = 108;
inline constexpr std::uint64_t kGetppid = 110;
inline constexpr std::uint64_t kArchPrctl = 158;
inline constexpr std::uint64_t kGettid = 186;
inline constexpr std::uint64_t kGetdents64 = 217;
inline constexpr std::uint64_t kSetTidAddress = 218;
inline constexpr std::uint64_t kClockGettime = 228;
inline constexpr std::uint64_t kExitGroup = 231;
inline constexpr std::uint64_t kTgkill = 234;
inline constexpr std::uint64_t kOpenat = 257;
inline constexpr std::uint64_t kNewfstatat = 262;
inline constexpr std::uint64_t kReadlinkat = 267;
inline constexpr std::uint64_t kSetRobustList = 273;
inline constexpr std::uint64_t kDup3 = 292;
inline constexpr std::uint64_t kPrlimit64 = 302;
inline constexpr std::uint64_t kGetrandom = 318;
inline constexpr std::uint64_t kStatx = 332;
} // namespace sysno

namespace arch_prctl_code {
inline constexpr std::uint64_t kSetGs = 0x1001;
inline constexpr std::uint64_t kSetFs = 0x1002;
inline constexpr std::uint64_t kGetFs = 0x1003;
inline constexpr std::uint64_t kGetGs = 0x1004;
} // namespace arch_prctl_code

namespace fcntl_cmd {
inline constexpr std::int32_t kDupfd = 0;
inline constexpr std::int32_t kGetfd = 1;
inline constexpr std::int32_t kSetfd = 2;
inline constexpr std::int32_t kGetfl = 3;
inline constexpr std::int32_t kSetfl = 4;
inline constexpr std::int32_t kDupfdCloexec = 1030;
} // namespace fcntl_cmd

// openat(2) flags (the subset this increment maps to the host). The values match
// the x86-64 Linux ABI; a guest passing them gets the kernel's interpretation.
namespace open_flags {
inline constexpr std::uint64_t kOAccmode = 0x3; // mask of the low access bits
inline constexpr std::uint64_t kORdonly = 0x0;
inline constexpr std::uint64_t kOWronly = 0x1;
inline constexpr std::uint64_t kORdwr = 0x2;
inline constexpr std::uint64_t kOCreat = 0x40;
inline constexpr std::uint64_t kOTrunc = 0x200;
inline constexpr std::uint64_t kOAppend = 0x400;
inline constexpr std::uint64_t kOExcl = 0x80;
// O_DIRECTORY: fail unless the path names a directory. Its *guest* value
// (0x10000) differs from the host's on AArch64 (0x4000), so it is translated
// explicitly rather than passed through.
inline constexpr std::uint64_t kODirectory = 0x10000;
// O_NONBLOCK: non-blocking I/O. Same value on both ABIs, but translated
// explicitly for the same reason.
inline constexpr std::uint64_t kONonblock = 0x800;
// O_CLOEXEC: accepted and ignored; the handler always opens with O_CLOEXEC so a
// guest descriptor never leaks into a host child.
inline constexpr std::uint64_t kOCloseexec = 0x80000;
} // namespace open_flags

// AT_FDCWD: a relative pathname is resolved against the current working
// directory. Only this value is supported; any other dirfd returns -ENOSYS
// (a real *at() with an explicit directory descriptor is not implemented).
inline constexpr std::uint64_t kAtFdcwd = static_cast<std::uint64_t>(-100);

// AT_SYMLINK_NOFOLLOW: do not dereference the final path component (lstat).
inline constexpr std::uint64_t kAtSymlinkNofollow = 0x100;

// statx(2) flags and the request/return mask. The values are the same on x86-64
// and AArch64 (a fixed UAPI), so they are translated as-is. Only
// AT_SYMLINK_NOFOLLOW is honoured among the flags; the mask is honoured by the
// host, which fills exactly the fields the guest asked for.
namespace statx {
inline constexpr std::uint64_t kAtEmptyPath = 0x1000;
inline constexpr std::uint64_t kBasicStats = 0x7ff; // STATX_BASIC_STATS
} // namespace statx

// Guest (x86-64) `struct statx` field offsets. Unlike `struct stat`, this
// structure is built entirely from fixed-width `__u32`/`__u64` fields (the
// kernel's UAPI forbids padding differences), so the layout is *identical* on
// x86-64 and AArch64. When the host permits statx(2) the kernel's bytes are
// therefore forwarded verbatim; where it does not - Android before API 30, see
// hyperion/host_syscalls.hpp - the same layout is filled from fstatat(2) by
// write_guest_statx_from_stat(). The offsets are recorded here so a test can
// assert them against the UAPI, and so both writers are explicit rather than
// unchecked struct copies.
namespace guest_statx {
inline constexpr std::size_t kSize = 256;         // sizeof(struct statx)
inline constexpr std::size_t kOffMask = 0;        // __u32 stx_mask
inline constexpr std::size_t kOffBlksize = 4;     // __u32 stx_blksize
inline constexpr std::size_t kOffAttributes = 8;  // __u64 stx_attributes
inline constexpr std::size_t kOffNlink = 16;      // __u32 stx_nlink
inline constexpr std::size_t kOffUid = 20;        // __u32 stx_uid
inline constexpr std::size_t kOffGid = 24;        // __u32 stx_gid
inline constexpr std::size_t kOffMode = 28;       // __u16 stx_mode (+1 pad)
inline constexpr std::size_t kOffIno = 32;        // __u64 stx_ino
inline constexpr std::size_t kOffSize = 40;       // __u64 stx_size
inline constexpr std::size_t kOffBlocks = 48;     // __u64 stx_blocks
inline constexpr std::size_t kOffAttrmask = 56;   // __u64 stx_attributes_mask
inline constexpr std::size_t kOffAtime = 64;      // struct statx_timestamp
inline constexpr std::size_t kOffBtime = 80;      // struct statx_timestamp
inline constexpr std::size_t kOffCtime = 96;      // struct statx_timestamp
inline constexpr std::size_t kOffMtime = 112;     // struct statx_timestamp
inline constexpr std::size_t kOffRdevMajor = 128; // __u32 stx_rdev_major
inline constexpr std::size_t kOffRdevMinor = 132; // __u32 stx_rdev_minor
inline constexpr std::size_t kOffDevMajor = 136;  // __u32 stx_dev_major
inline constexpr std::size_t kOffDevMinor = 140;  // __u32 stx_dev_minor
} // namespace guest_statx

// Guest (x86-64) `struct stat` field offsets. The host layout differs on
// AArch64, so fstat() builds the guest layout field by field rather than
// copying the host struct. Offsets are for the x86-64 LP64 layout.
namespace guest_stat {
inline constexpr std::size_t kSize = 144;        // sizeof(struct stat) on x86-64
inline constexpr std::size_t kOffStDev = 0;      // dev_t st_dev (8)
inline constexpr std::size_t kOffStIno = 8;      // ino_t st_ino (8)
inline constexpr std::size_t kOffStNlink = 16;   // nlink_t st_nlink (8)
inline constexpr std::size_t kOffStMode = 24;    // mode_t st_mode (4)
inline constexpr std::size_t kOffStUid = 28;     // uid_t st_uid (4)
inline constexpr std::size_t kOffStGid = 32;     // gid_t st_gid (4)
inline constexpr std::size_t kOffStRdev = 40;    // dev_t st_rdev (8)
inline constexpr std::size_t kOffStSize = 48;    // off_t st_size (8)
inline constexpr std::size_t kOffStBlksize = 56; // blksize_t st_blksize (8)
inline constexpr std::size_t kOffStBlocks = 64;  // blkcnt_t st_blocks (8)
inline constexpr std::size_t kOffStAtime = 72;   // struct timespec st_atim (16)
inline constexpr std::size_t kOffStMtime = 88;   // struct timespec st_mtim (16)
inline constexpr std::size_t kOffStCtime = 104;  // struct timespec st_ctim (16)
} // namespace guest_stat

// Guest PROT_* and MAP_* flags (the subset this increment honours). The values
// match the x86-64 Linux ABI, so a guest passing them gets the kernel's
// interpretation, not a reinterpretation.
namespace mmap_flags {
inline constexpr std::uint64_t kProtNone = 0x0;
inline constexpr std::uint64_t kProtRead = 0x1;
inline constexpr std::uint64_t kProtWrite = 0x2;
inline constexpr std::uint64_t kProtExec = 0x4;
inline constexpr std::uint64_t kMapShared = 0x01;
inline constexpr std::uint64_t kMapPrivate = 0x02;
inline constexpr std::uint64_t kMapAnonymous = 0x20;
inline constexpr std::uint64_t kMapFixed = 0x10;
} // namespace mmap_flags

// lseek(2) whence values.
namespace seek {
inline constexpr std::uint64_t kSet = 0;
inline constexpr std::uint64_t kCur = 1;
inline constexpr std::uint64_t kEnd = 2;
} // namespace seek

// clone(2) flags, x86-64 values. See COMPATIBILITY.md: HYPERION implements the
// *process-creating* subset only. A flag that asks for shared state the guest
// model cannot provide (a shared address space, a shared fd table, a thread
// group) is rejected with -ENOSYS rather than silently downgraded to a fork,
// because a program that asked for CLONE_VM will corrupt its own memory if it
// gets a private copy instead.
namespace clone_flags {
inline constexpr std::uint64_t kVm = 0x00000100;
inline constexpr std::uint64_t kFs = 0x00000200;
inline constexpr std::uint64_t kFiles = 0x00000400;
inline constexpr std::uint64_t kSighand = 0x00000800;
inline constexpr std::uint64_t kThread = 0x00010000;
inline constexpr std::uint64_t kNewns = 0x00020000;
inline constexpr std::uint64_t kSysvsem = 0x00040000;
inline constexpr std::uint64_t kSetTid = 0x00080000;
inline constexpr std::uint64_t kParentSetTid = 0x00100000;
inline constexpr std::uint64_t kUntraced = 0x00800000;
inline constexpr std::uint64_t kChildCleartid = 0x00200000;
inline constexpr std::uint64_t kChildSetTid = 0x01000000;
inline constexpr std::uint64_t kPtrace = 0x00002000;
inline constexpr std::uint64_t kVfork = 0x00004000;
inline constexpr std::uint64_t kStop = 0x00000002;
inline constexpr std::uint64_t kNewuts = 0x04000000;
inline constexpr std::uint64_t kNewipc = 0x08000000;
inline constexpr std::uint64_t kNewuser = 0x10000000;
inline constexpr std::uint64_t kNewpid = 0x20000000;
inline constexpr std::uint64_t kNewnet = 0x40000000;
inline constexpr std::uint64_t kIo = 0x80000000ULL;

// Exit signal occupies the low 8 bits of the clone flags word.
inline constexpr std::uint64_t kExitSignalMask = 0xFF;
} // namespace clone_flags

// wait4(2) options.
namespace wait_options {
inline constexpr std::int32_t kNoHang = 0x00000001;
inline constexpr std::int32_t kUntraced = 0x00000002;
inline constexpr std::int32_t kContinued = 0x00000008;
} // namespace wait_options

// SIGCHLD, the exit signal glibc/musl pass to clone(2) for a subprocess.
inline constexpr std::uint64_t kSigchld = 17;

// Negative errno values as the guest observes them (returned in rax).
inline constexpr std::int64_t kEperm = 1;
inline constexpr std::int64_t kEnoent = 2;
inline constexpr std::int64_t kEsrch = 3;
inline constexpr std::int64_t kEio = 5;
inline constexpr std::int64_t kEbadf = 9;
inline constexpr std::int64_t kEchild = 10;
inline constexpr std::int64_t kEagain = 11;
inline constexpr std::int64_t kEnomem = 12;
inline constexpr std::int64_t kEacces = 13;
inline constexpr std::int64_t kEfault = 14;
inline constexpr std::int64_t kEexist = 17;
inline constexpr std::int64_t kEnotdir = 20;
inline constexpr std::int64_t kEisdir = 21;
inline constexpr std::int64_t kEinval = 22;
inline constexpr std::int64_t kEnfile = 23;
inline constexpr std::int64_t kEmfile = 24;
inline constexpr std::int64_t kEspipe = 29;
inline constexpr std::int64_t kEnosys = 38;
inline constexpr std::int64_t kEoverflow = 75;
inline constexpr std::int64_t kEloop = 40;
inline constexpr std::int64_t kEnametoolong = 36;
inline constexpr std::int64_t kErange = 34;
inline constexpr std::int64_t kEintr = 4;
inline constexpr std::int64_t kEnotty = 25;
inline constexpr std::int64_t kEtimeout = 110;

// What the handler did with a syscall.
enum class SyscallStatus : std::uint8_t {
    kHandled,       // emulated: rax holds the result (or a negative errno)
    kUnimplemented, // not implemented: rax = -ENOSYS, execution continues
    kExit,          // the guest asked to terminate
};

const char* to_string(SyscallStatus status);

struct SyscallResult {
    SyscallStatus status = SyscallStatus::kUnimplemented;
    std::uint64_t number = 0;   // the syscall number requested
    std::int64_t exit_code = 0; // valid when status == kExit
};

// A recorded guest signal disposition (from rt_sigaction(13)).
struct GuestSignalAction {
    std::uint64_t handler = 0; // SIG_DFL (0), SIG_IGN (1), or a guest address
    std::uint64_t flags = 0;
    std::uint64_t restorer = 0;
    std::uint64_t mask = 0;
    bool installed = false;
};

// The guest system-call interface. An implementation receives the guest state
// with rax = number and rdi/rsi/rdx/r10/r8/r9 = arguments, and must write the
// result into rax.
class SyscallHandler {
public:
    virtual ~SyscallHandler() = default;
    virtual SyscallResult handle(GuestState& state, Memory& memory) = 0;

    // Guest signal-delivery interface. Default implementations mean "no signals
    // are ever pending and nothing is masked", which is correct for handlers
    // that do not implement signal support. MinimalSyscallHandler overrides
    // these to provide real guest signal delivery.
    virtual std::uint64_t guest_sigmask() const noexcept { return 0; }
    virtual void clear_pending_signal(int sig) noexcept {}
    virtual std::size_t sigaction_count() const noexcept { return 0; }
    virtual const GuestSignalAction& sigaction_at(std::size_t index) const noexcept {
        static const GuestSignalAction kDefault{}; // SIG_DFL, no handler
        return kDefault;
    }
    virtual std::uint64_t signal_pending_for_delivery() const noexcept { return 0; }
};

// Shared POSIX open file description (shared across dup/dup2/dup3/fcntl F_DUPFD).
// Regular files opened via openat(2) store an unbuffered kernel `host_fd` so
// read/write/lseek/pread/pwrite/fstat/getdents64 never suffer C stdio buffer
// desynchronisation; adopted `std::FILE*` streams (such as in-memory test
// streams) keep `stream` and borrow ownership from the caller.
struct OpenFileDescription {
    int host_fd = -1;
    bool owned_fd = false;
    int sink_fd = -1; // 1 or 2 when routed through OutputSink
    std::FILE* stream = nullptr;
    int status_flags = 0;

    ~OpenFileDescription();
    OpenFileDescription() = default;
    OpenFileDescription(const OpenFileDescription&) = delete;
    OpenFileDescription& operator=(const OpenFileDescription&) = delete;
};

// Per-descriptor slot in the guest process's file descriptor table. `cloexec`
// is per-FD (FD_CLOEXEC), whereas file offset and status flags live on `desc`.
struct GuestDescriptor {
    std::shared_ptr<OpenFileDescription> desc;
    bool cloexec = false;

    bool is_open() const noexcept { return desc != nullptr; }
};

// A child process HYPERION created on the guest's behalf.
//
// A guest process is a *real host process* plus its own private copy of the
// guest execution state. `guest_pid` is the host pid, which is also the pid the
// guest sees: getpid(2) already forwards to the host, so a guest that compares
// `fork()`'s return value against its child's `getpid()` gets a consistent
// answer without a pid translation table.
struct ChildProcess {
    int guest_pid = 0;
    bool reaped = false;
    int raw_status = 0; // host wait(2) status, already in the guest's encoding
};

// A handler for the minimum needed to run a self-contained guest program that
// prints, reads, allocates memory and terminates. `write` goes to a host sink
// (stdout for fd 1, stderr for fd 2); `read`/`close`/`lseek`/`openat`/`fstat`
// operate on a small guest descriptor table the embedder can seed with adopted
// host descriptors (fd 0 is stdin by default); `mmap`/`mprotect`/`munmap`/`brk`
// operate on the supplied GuestAddressSpace; `exit`/`exit_group` stop the
// program; every other number returns -ENOSYS.
//
// The output sink is injectable so tests can capture guest output without
// touching the test runner's own stdout. The signature mirrors write(2): it
// returns the number of bytes written, or -1 on error.
//
// The address space is supplied by the caller (not owned), so the same
// GuestAddressSpace can be shared with the interpreter or translation cache and
// the handler has no hidden global state. When it is null, the memory syscalls
// return -ENOSYS rather than pretending to succeed.
//
// The descriptor table is owned by the handler (a guest file descriptor is
// handler-local state, like a real process's table). It is not a host file
// descriptor table: the handler maps guest fd -> host FILE* and enforces the
// mapping. `read`/`write` refuse a guest fd that is not in the table with
// -EBADF, so a guest cannot name an arbitrary host descriptor. openat() opens a
// host file on the guest's behalf and allocates a guest fd for it; the stream is
// then handler-owned and closed when the guest closes the fd.
class SyscallDispatcher;
struct RegistryOptions;
struct SyscallAccess;
struct SyscallContext;

class MinimalSyscallHandler final : public SyscallHandler {
public:
    using OutputSink = std::function<long(int fd, const std::uint8_t* data, std::size_t count)>;

    // With no sink, output is written to the process's stdout/stderr. With no
    // address space, mmap/mprotect/munmap/brk are unimplemented. The descriptor
    // table starts with fd 0/1/2 mapped to the process's stdin/stdout/stderr;
    // see adopt() to add an already-open host stream under a guest fd.
    explicit MinimalSyscallHandler(OutputSink sink = {},
                                   GuestAddressSpace* address_space = nullptr);
    ~MinimalSyscallHandler() override;
    MinimalSyscallHandler(const MinimalSyscallHandler&) = delete;
    MinimalSyscallHandler& operator=(const MinimalSyscallHandler&) = delete;

    SyscallResult handle(GuestState& state, Memory& memory) override;

    // Map an already-open host stream to a guest descriptor so a guest can read
    // from it. This lets an embedder grant access to a specific host stream
    // without the guest naming a path. Ownership stays with the caller (the
    // FILE* is not fclose'd by the handler), matching the "borrowed, not owned"
    // boundary. Fails (false) if the fd is already taken or out of range.
    bool adopt(int guest_fd, std::FILE* stream);

    // Enable full Box64-style Linux user-space compatibility (`mmap` file-backed
    // shared libraries + `MAP_FIXED`, directory-relative `*at(dirfd, ...)` calls,
    // `ioctl(TCGETS/TIOCGWINSZ)`, `open`/`stat`/`lstat`/`pipe2`/`futex`/signals/
    // `sched_getaffinity`/`sysinfo`/`prctl`, and optional `--sysroot` prefixing).
    void enable_linux_compat(std::string sysroot = {});

    // --- persistent syscall registry ---------------------------------------
    //
    // The dispatcher always holds the compiled-in rules. This call additionally
    // loads the persistent external registry (see syscall_registry.hpp for the
    // path resolution order), validates it and compiles it into the dispatch
    // table. It is a single startup operation: nothing is read from disk, parsed
    // or allocated per syscall afterwards.
    //
    // It is opt-in rather than automatic so that constructing a handler in a
    // test or an embedding application has no filesystem side effects. The
    // `hyperion run` command enables it unless --no-syscall-registry is given.
    void enable_syscall_registry(); // default options
    void enable_syscall_registry(const RegistryOptions& options);

    // Re-read and re-validate the registry, then atomically swap the compiled
    // snapshot into service. A failed load leaves the previous rules in service.
    // Returns the report status as an integer (0 == ok) and fills `message`.
    int reload_syscall_registry(const RegistryOptions& options, std::string& message);

    // The dispatcher, for diagnostics (`hyperion syscall-registry`). Never null.
    const SyscallDispatcher& dispatcher() const noexcept;

    // --- process support -----------------------------------------------------
    //
    // fork(2)/vfork(2)/clone(2) create a real host process that runs a *private
    // copy* of the guest state to completion; wait4(2) reaps it. See
    // src/core/process.cpp and COMPATIBILITY.md for exactly which semantics are
    // honoured and which clone flags are rejected.

    // Guest-visible pid of the process the guest is running as. Defaults to the
    // host pid, which is what getpid(2) already reports, so a guest sees one
    // coherent pid space. Overridable for tests that need a fixed value.
    void set_guest_pid(int pid) { guest_pid_ = pid; }
    int guest_pid() const noexcept { return guest_pid_; }

    // Execution tier used for the guest side of a forked child. Tier 0 (the
    // reference interpreter) is the default because it is available on every
    // host and is the tier the differential test validates tier 1 against.
    void set_child_tier(int tier) { child_tier_ = (tier == 1) ? 1 : 0; }
    int child_tier() const noexcept { return child_tier_; }

    // Instruction limit for a forked child's guest execution.
    void set_child_max_steps(std::uint64_t steps) { child_max_steps_ = steps; }
    std::uint64_t child_max_steps() const noexcept { return child_max_steps_; }

    // Read-only view of the guest descriptor table, for tests.
    const std::vector<GuestDescriptor>& descriptors() const noexcept { return files_; }
    const std::vector<ChildProcess>& children() const noexcept { return children_; }

    // The image the guest was loaded from. This is what `/proc/self/exe` has to
    // report: the guest is running *inside* this host process, so the host's own
    // /proc/self/exe names the translator, and a guest that re-executes itself
    // through that path (BusyBox does exactly this to run an applet) would end up
    // exec'ing the translator instead of itself.
    void set_guest_image_path(std::string path) { guest_image_path_ = std::move(path); }
    const std::string& guest_image_path() const noexcept { return guest_image_path_; }

    // --- signal-state accessors for signal_handling.cpp ---------------------
    // The guest signal mask (bit sig-1 set = blocked). Managed by rt_sigprocmask
    // (14) in src/core/syscall.cpp. Named guest_sigmask to avoid clashing with
    // the deprecated ::sigmask() C library function.
    std::uint64_t guest_sigmask() const noexcept { return sigmask_; }
    // Clear the pending bit for `sig` after it has been dequeued for delivery.
    void clear_pending_signal(int sig) noexcept;
    // Number of guest signal-action slots (kMaxSignal + 1).
    std::size_t sigaction_count() const noexcept { return kMaxSignal + 1; }
    // Read-only access to a recorded signal disposition. Bounds-checked by the
    // caller (index < sigaction_count()).
    const GuestSignalAction& sigaction_at(std::size_t index) const noexcept {
        return sigactions_[index];
    }
    // Signals raised synchronously (kill/tgkill) and awaiting delivery.
    std::uint64_t signal_pending_for_delivery() const noexcept {
        return signal_pending_;
    }

private:
    // process.cpp implements fork/vfork/clone/execve/wait4 against this state.
    friend struct ProcessAccess;
    // syscall_dispatch.cpp / syscall_adapters.cpp provide the compatibility
    // services an adapter needs (descriptor table, compat mode, sysroot).
    friend struct SyscallAccess;

    // The real dispatch. handle() wraps it so HYPERION_TRACE_SYSCALLS=1 can log
    // every call and its result without duplicating the switch.
    SyscallResult handle_unguarded(GuestState& state, Memory& memory);

    std::unique_ptr<SyscallDispatcher> dispatcher_;
    OutputSink sink_;
    GuestAddressSpace* address_space_;
    // Guest fd -> descriptor. Indexed by guest fd, so the guest can never index
    // out of range (the lookup is bounds-checked).
    std::vector<GuestDescriptor> files_;
    std::uint64_t clear_child_tid_ = 0;
    std::uint64_t robust_list_head_ = 0;
    bool linux_compat_ = false;
    std::string sysroot_;
    std::uint64_t sigmask_ = 0;
    // Installed signal dispositions, exactly as the guest wrote them (the x86-64
    // kernel's 32-byte `struct sigaction`: handler, flags, restorer, mask).
    // Recording them is what makes sigaction()/signal() round-trip: a program
    // that installs a handler and reads it back sees what it installed, instead
    // of the zeroed block this layer used to hand back. Delivery is a separate
    // mechanism and is not implemented - see COMPATIBILITY.md.
    static constexpr std::size_t kMaxSignal = 64;
    std::array<GuestSignalAction, kMaxSignal + 1> sigactions_{};
    // sigaltstack(2): the guest's alternate signal stack, as it wrote it
    // (24-byte stack_t: ss_sp, ss_flags, ss_size).
    std::uint64_t altstack_sp_ = 0;
    std::int32_t altstack_flags_ = 2; // SS_DISABLE
    std::uint64_t altstack_size_ = 0;

    // --- Signals raised at this guest, and what happens to them (P12) --------
    //
    // Only *synchronous* self-raised signals exist here: kill/tgkill aimed at
    // this process. Nothing arrives asynchronously (no host signal is forwarded),
    // so `signal_pending_` only ever holds signals that arrived while blocked.
    // Bit (sig - 1) is set for a pending signal, matching the guest's own
    // sigset_t layout.
    std::uint64_t signal_pending_ = 0;
    // Mask-aware: queues a blocked signal, returns 0 when the guest keeps
    // running, else 128 + sig.
    std::int64_t raise_signal(int sig);
    // Re-evaluate signals that were blocked when they arrived; call after the
    // mask changes. Same return convention as raise_signal().
    std::int64_t deliver_pending_signals();

    // prctl(2) state this layer can answer from. These are the options whose
    // observable meaning for the guest is "what does prctl report back":
    // PR_SET/GET_NAME, PR_SET/GET_PDEATHSIG, PR_SET/GET_DUMPABLE. The two that
    // can be translated to the *host* for real - PR_SET_NO_NEW_PRIVS and
    // PR_SET_TIMERSLACK - are forwarded; see the prctl case in
    // src/core/syscall.cpp for the full table and what is refused.
    std::array<char, 16> process_name_{}; // PR_SET_NAME, always NUL-terminated
    std::int32_t pdeathsig_ = 0;
    std::int32_t dumpable_ = 1; // the kernel's default

    // --- process state ------------------------------------------------------
    std::vector<ChildProcess> children_; // children this guest created
    int guest_pid_ = 0;                  // 0 = "use the host pid"
    int child_tier_ = 0;
    std::uint64_t child_max_steps_ = 1'000'000'000ULL;
    // argv[0] and the image path of the running guest, so execve can resolve a
    // relative program name and so a child can rebuild its own command line.
    std::string guest_argv0_;
    std::string guest_image_path_;
    // Set when this handler instance is running inside a forked guest child and
    // must terminate the host process rather than unwind into the parent's loop.
    bool in_guest_child_ = false;
    // vfork(2) suspends the parent until the child execs or exits. The parent
    // blocks on this pipe; the child signals it from its exec/exit path.
    int vfork_sync_fd_ = -1;
};

// True if MinimalSyscallHandler implements this syscall number (as opposed to
// returning -ENOSYS). Used by tests and by the compatibility matrix.
//
// This covers the *statically compiled* arms of the handler (the switch in
// src/core/syscall.cpp plus, through is_linux_compat_syscall, the arms that are
// only reachable in --linux-compat mode). Syscalls reached through the
// compatibility-adapter layer are reported by is_compat_adapter_syscall() in
// syscall_dispatch.hpp instead; the two sets are disjoint and both are checked
// by the syscall inventory CI gate.
bool is_minimal_syscall(std::uint64_t number);

// True if the number is handled by the Box64-style --linux-compat arm of the
// handler (the nested switch that runs when enable_linux_compat() was called).
// These are *not* part of is_minimal_syscall(), which is why the registry
// validator consults both before allowing a rule to claim a number.
bool is_linux_compat_syscall(std::uint64_t number);

} // namespace hyperion

#endif // HYPERION_SYSCALL_HPP
