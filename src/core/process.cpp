// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest process creation: fork(2), vfork(2), the process-creating subset of
// clone(2), execve(2) and wait4(2).
//
// THE MODEL
// ---------
// HYPERION runs a guest as an ordinary unprivileged host process. A guest
// *process* is therefore a real host process plus its own private copy of the
// guest execution state. fork(2) does a real host fork(); the child then runs
// the guest from the instruction following the `syscall`, and terminates its
// host process with the guest's exit status. The parent records the child's pid
// and reaps it with wait4(2).
//
// Two properties of this codebase make that work, and both were verified rather
// than assumed:
//
//   1. Both execution tiers advance `rip` past the `syscall` instruction
//      *before* invoking the handler (interpreter.cpp Opcode::kSyscall,
//      translation_cache.cpp BlockResult::kSyscall). A child can therefore
//      resume at `state` with no program-counter fixup.
//
//   2. Guest pages are backed by host heap storage owned by GuestAddressSpace
//      (`Region::bytes`). A host fork alone would leave parent and child sharing
//      those same vectors until either side mmap/munmaps and reallocates them,
//      invalidating the other's cached host page pointers. So the child is
//      given a *deep copy* of the address space (GuestAddressSpace's copy
//      constructor) and only then starts executing.
//
// WHAT IS DELIBERATELY NOT IMPLEMENTED
// ------------------------------------
// Thread creation. CLONE_VM / CLONE_THREAD / CLONE_FILES / CLONE_FS /
// CLONE_SIGHAND are rejected with -ENOSYS instead of being quietly downgraded
// to a fork: a guest that asked for a shared address space and received a
// private copy would corrupt its own memory, which is worse than a clean error.
// Namespace flags are rejected because creating a namespace needs CAP_SYS_ADMIN,
// which unrooted Android/Termux does not have. See COMPATIBILITY.md.
//
// No fake success: every unsupported flag, bad pointer and foreign pid returns
// the errno a real kernel would, and leaves guest state untouched.
#include "hyperion/process.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hyperion/address_space.hpp"
#include "hyperion/elf_loader.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/translation_cache.hpp"

// WNOHANG/WUNTRACED/WCONTINUED are POSIX wait(2) flags and carry the same values
// on x86-64 and AArch64, so the guest's options word is forwarded to the host
// unchanged. Pinned here so a host with different values fails the build instead
// of silently misinterpreting a guest's flags.
static_assert(WNOHANG == 1, "guest wait4 options assume WNOHANG == 1");
static_assert(WUNTRACED == 2, "guest wait4 options assume WUNTRACED == 2");
static_assert(WCONTINUED == 8, "guest wait4 options assume WCONTINUED == 8");
static_assert(SIGCHLD == 17, "x86-64 SIGCHLD is 17 on every supported host");
static_assert(SIGKILL == 9, "x86-64 SIGKILL is 9 on every supported host");
static_assert(SIGSTOP == 19, "x86-64 SIGSTOP is 19 (it is 17 on AArch64)");

namespace hyperion {

// Granted access to MinimalSyscallHandler's private process state. A struct
// rather than a long friend list so everything process support may touch stays
// in one readable place.
struct ProcessAccess {
    static std::vector<GuestDescriptor>& files(MinimalSyscallHandler& h) { return h.files_; }
    static std::vector<ChildProcess>& children(MinimalSyscallHandler& h) { return h.children_; }
    static GuestAddressSpace* address_space(MinimalSyscallHandler& h) { return h.address_space_; }
    static const std::string& sysroot(MinimalSyscallHandler& h) { return h.sysroot_; }
    static std::string& guest_argv0(MinimalSyscallHandler& h) { return h.guest_argv0_; }
    static std::string& guest_image_path(MinimalSyscallHandler& h) { return h.guest_image_path_; }
};

namespace {

// --- guest register arguments ------------------------------------------------

// Mirrors the tracer gate in syscall.cpp: HYPERION_TRACE_SYSCALLS=1 turns on
// per-call logging there, and execve logs the image it actually resolved here,
// because that single value is what decides whether a guest re-executes itself
// or accidentally executes the translator.
bool syscall_trace_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("HYPERION_TRACE_SYSCALLS");
        return v != nullptr && v[0] != '0' && v[0] != '\0';
    }();
    return enabled;
}

std::uint64_t proc_arg(const GuestState& s, int index) {
    constexpr Gpr kArgRegs[6] = {Gpr::kRdi, Gpr::kRsi, Gpr::kRdx, Gpr::kR8, Gpr::kR9, Gpr::kR10};
    // The x86-64 syscall ABI passes arguments in rdi, rsi, rdx, r10, r8, r9
    // (r10, not rcx, because `syscall` clobbers rcx with the return address).
    constexpr int kOrder[6] = {0, 1, 2, 5, 3, 4};
    return s.gpr[static_cast<std::size_t>(kArgRegs[kOrder[index]])];
}

std::int32_t proc_arg_i32(const GuestState& s, int index) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(proc_arg(s, index)));
}

void put_le(std::uint8_t* buf, std::size_t off, std::uint64_t value, std::size_t bytes) {
    for (std::size_t b = 0; b < bytes; ++b) {
        buf[off + b] = static_cast<std::uint8_t>((value >> (8 * b)) & 0xFFU);
    }
}

std::uint64_t get_le(const std::uint8_t* buf, std::size_t off, std::size_t bytes) {
    std::uint64_t v = 0;
    for (std::size_t b = bytes; b > 0; --b) {
        v = (v << 8) | buf[off + b - 1];
    }
    return v;
}

// --- guest strings -----------------------------------------------------------

// Read a NUL-terminated guest string. Returns 0, -EFAULT on an unmapped byte,
// -ENAMETOOLONG when no terminator appears within the limit.
std::int64_t read_guest_string(Memory& memory, std::uint64_t address, std::string& out) {
    out.clear();
    if (address == 0) {
        return -kEfault;
    }
    constexpr std::size_t kMaxLen = 4096; // PATH_MAX-equivalent guard
    for (std::size_t i = 0; i < kMaxLen; ++i) {
        std::uint8_t byte = 0;
        if (!memory.read(address + i, &byte, 1)) {
            return -kEfault;
        }
        if (byte == 0) {
            return 0;
        }
        out.push_back(static_cast<char>(byte));
    }
    return -kEnametoolong;
}

// Read a NULL-terminated array of guest strings (argv/envp). A NULL array
// pointer is an empty list, not a fault, which is what execve accepts.
std::int64_t read_guest_string_array(Memory& memory, std::uint64_t array_addr,
                                     std::vector<std::string>& out) {
    out.clear();
    if (array_addr == 0) {
        return 0;
    }
    constexpr std::size_t kMaxEntries = 4096;
    for (std::size_t i = 0; i < kMaxEntries; ++i) {
        std::uint8_t raw[8] = {};
        if (!memory.read(array_addr + i * 8, raw, sizeof(raw))) {
            return -kEfault;
        }
        const std::uint64_t ptr = get_le(raw, 0, 8);
        if (ptr == 0) {
            return 0;
        }
        std::string s;
        const std::int64_t rc = read_guest_string(memory, ptr, s);
        if (rc != 0) {
            return rc;
        }
        out.emplace_back(std::move(s));
    }
    return -7; // -E2BIG, the kernel's answer for an oversized argv/envp
}

// --- execve path resolution --------------------------------------------------

std::string prefix_sysroot(const std::string& path, const std::string& sysroot) {
    if (sysroot.empty() || path.empty() || path[0] != '/') {
        return path;
    }
    return sysroot + path;
}

bool read_whole_file(const std::string& path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    std::uint8_t buf[8192];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    const bool ok = std::feof(f) != 0;
    std::fclose(f);
    return ok;
}

std::int64_t try_exec_candidate(const std::string& path, std::string& resolved,
                                std::vector<std::uint8_t>& bytes) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEnoent : errno);
    }
    if (S_ISDIR(st.st_mode)) {
        return -kEisdir;
    }
    if (!S_ISREG(st.st_mode)) {
        return -kEacces;
    }
    if (::access(path.c_str(), X_OK) != 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEacces : errno);
    }
    if (!read_whole_file(path, bytes) || bytes.empty()) {
        return -kEio;
    }
    resolved = path;
    return 0;
}

// `/proc/self/exe` and `/proc/<own pid>/exe` name the *running host process*,
// which for a guest is the translator rather than the guest image. BusyBox uses
// that path to re-execute itself when running an applet, so resolving it to the
// host would make the guest exec the translator and its own arguments would be
// reinterpreted as translator commands. Every user-mode translator has to
// redirect this one path; returning the guest's own image is the only answer
// that lets a self-re-executing guest work.
bool is_self_exe_path(const std::string& path, int guest_pid) {
    if (path == "/proc/self/exe" || path == "/proc/thread-self/exe") {
        return true;
    }
    if (guest_pid > 0 && path.rfind("/proc/", 0) == 0 && path.size() > 10 &&
        path.compare(path.size() - 4, 4, "/exe") == 0) {
        const std::string pid_part = path.substr(6, path.size() - 10);
        if (!pid_part.empty() && pid_part.find_first_not_of("0123456789") == std::string::npos) {
            return std::strtol(pid_part.c_str(), nullptr, 10) == guest_pid;
        }
    }
    return false;
}

// execve(2) program-name resolution: a path containing '/' is used as-is
// (sysroot-prefixed when a sysroot is configured); a bare name is searched on
// PATH. Only executable regular files are accepted.
//
// The guest's own environment is not retained by the handler after startup, so
// the host PATH is searched. It is the same PATH the guest inherited at startup
// unless the guest called setenv(3) on it since; documented in COMPATIBILITY.md.
std::int64_t resolve_exec_path(const std::string& name, const std::string& sysroot,
                               const std::string& guest_image, int guest_pid, std::string& resolved,
                               std::vector<std::uint8_t>& bytes) {
    if (is_self_exe_path(name, guest_pid)) {
        // The guest asked to run itself. Handing back the host's /proc/self/exe
        // would exec the translator; hand back the guest's own image instead.
        if (guest_image.empty()) {
            return -kEnoent;
        }
        return try_exec_candidate(guest_image, resolved, bytes);
    }
    if (name.find('/') != std::string::npos) {
        return try_exec_candidate(prefix_sysroot(name, sysroot), resolved, bytes);
    }

    const char* path_env = std::getenv("PATH");
    const std::string search = (path_env != nullptr && path_env[0] != '\0') ? path_env : ".";

    std::int64_t last_error = -kEnoent;
    std::size_t start = 0;
    while (true) {
        const std::size_t sep = search.find(':', start);
        const std::size_t end = (sep == std::string::npos) ? search.size() : sep;
        const std::string dir = search.substr(start, end - start);
        if (!dir.empty()) {
            const std::int64_t rc =
                try_exec_candidate(prefix_sysroot(dir + "/" + name, sysroot), resolved, bytes);
            if (rc == 0) {
                return 0;
            }
            // ENOENT/ENOTDIR keep the search going; anything else (EACCES on a
            // file that exists, EIO reading it) is what execve reports.
            if (rc != -kEnoent && rc != -kEnotdir) {
                last_error = rc;
            }
        }
        if (sep == std::string::npos) {
            break;
        }
        start = sep + 1;
    }
    return last_error;
}

// --- descriptor-table handling ----------------------------------------------

// Copy the guest descriptor table the way fork(2) does: the descriptors are
// *shared* (same open file description, so a shared file offset and shared
// status flags) while the FD_CLOEXEC bits are per-descriptor and copied by
// value. Sharing the OpenFileDescription is what makes a pipe created before the
// fork carry data between the two processes.
std::vector<GuestDescriptor> fork_descriptor_table(const std::vector<GuestDescriptor>& parent) {
    std::vector<GuestDescriptor> child;
    child.reserve(parent.size());
    for (const GuestDescriptor& d : parent) {
        child.push_back(GuestDescriptor{d.desc, d.cloexec});
    }
    return child;
}

// execve(2): drop every descriptor marked FD_CLOEXEC. The slot is cleared so the
// shared_ptr is released here; a descriptor that was dup()ed shares one
// OpenFileDescription, and clearing every cloexec slot is what finally closes
// the underlying host descriptor. Borrowed streams (fd 0/1/2 adopted from the
// host) are never owned, so stdio survives an exec exactly as it must.
void close_cloexec_descriptors(std::vector<GuestDescriptor>& files) {
    for (GuestDescriptor& d : files) {
        if (d.cloexec && d.desc != nullptr) {
            d.desc.reset();
            d.cloexec = false;
        }
    }
}

// Host descriptors opened on the guest's behalf are created O_CLOEXEC so they
// cannot leak into a host child. As a second line of defence, mark every owned
// host descriptor close-on-exec before forking, in case one was adopted without
// that flag.
void mark_owned_fds_cloexec(const std::vector<GuestDescriptor>& files) {
    for (const GuestDescriptor& d : files) {
        if (d.desc == nullptr || d.desc->host_fd < 0) {
            continue;
        }
        const int fl = ::fcntl(d.desc->host_fd, F_GETFD);
        if (fl >= 0 && (fl & FD_CLOEXEC) == 0) {
            (void)::fcntl(d.desc->host_fd, F_SETFD, fl | FD_CLOEXEC);
        }
    }
}

// --- clone(2) flag classification -------------------------------------------

// Flags HYPERION honours for a process-creating clone.
constexpr std::uint64_t kSupportedCloneFlags =
    clone_flags::kChildSetTid | clone_flags::kChildCleartid | clone_flags::kParentSetTid |
    clone_flags::kSetTid | clone_flags::kUntraced | clone_flags::kVfork;

// Flags that ask for shared state the guest model cannot provide, or for a
// kernel feature unrooted Android does not grant. Rejected rather than
// approximated: silently forking instead of sharing an address space would give
// the guest a private copy it believes is shared.
constexpr std::uint64_t kUnsupportedCloneFlags =
    clone_flags::kVm | clone_flags::kFs | clone_flags::kFiles | clone_flags::kSighand |
    clone_flags::kThread | clone_flags::kSysvsem | clone_flags::kPtrace | clone_flags::kStop |
    clone_flags::kNewns | clone_flags::kNewuts | clone_flags::kNewipc | clone_flags::kNewuser |
    clone_flags::kNewpid | clone_flags::kNewnet | clone_flags::kIo;

// x86-64 `struct rusage`: two `struct timeval` then 14 `long` counters, 8 bytes
// each on both x86-64 and AArch64 LP64. Written field by field rather than
// memcpy'd, so the layout is explicit and testable.
namespace guest_rusage {
constexpr std::size_t kSize = 144;
constexpr std::size_t kMaxRss = 32;
constexpr std::size_t kMinFlt = 40;
constexpr std::size_t kMajFlt = 56;
constexpr std::size_t kInBlock = 72;
constexpr std::size_t kOuBlock = 88;
constexpr std::size_t kNvCs = 128;
constexpr std::size_t kNivCs = 136;
} // namespace guest_rusage

} // namespace

namespace process {

// --- child side --------------------------------------------------------------

[[noreturn]] void run_child_prologue(const GuestState& child_state,
                                     const GuestAddressSpace& parent_space,
                                     const std::vector<GuestDescriptor>& parent_files,
                                     const std::string& sysroot, int tier, std::uint64_t max_steps,
                                     const ChildCloneSpec& spec, int vfork_write_fd) {
    // Deep copy: the child gets guest memory that is genuinely its own, so a
    // later mmap/munmap on either side cannot invalidate the other's cached host
    // page pointers.
    GuestAddressSpace space(parent_space);
    GuestState state = child_state;

    // CLONE_CHILD_SETTID writes the child's tid into the child's own memory, and
    // CLONE_SETTLS sets its %fs base. Both must land before the guest's first
    // instruction in the child runs.
    if ((spec.flags & clone_flags::kChildSetTid) != 0 && spec.child_tidptr != 0) {
        std::array<std::uint8_t, 4> raw{};
        put_le(raw.data(), 0, static_cast<std::uint64_t>(::getpid()), 4);
        (void)space.write(spec.child_tidptr, raw.data(), raw.size());
    }
    if ((spec.flags & clone_flags::kSetTid) != 0 && spec.tls != 0) {
        state.fs_base = spec.tls;
    }

    MinimalSyscallHandler handler({}, &space);
    ProcessAccess::files(handler) = parent_files;
    handler.enable_linux_compat(sysroot);

    const RunResult result = (tier == 1) ? TranslationCache().run(space, state, max_steps, &handler)
                                         : run_program(space, state, max_steps, &handler);

    // CLONE_CHILD_CLEARTID: the kernel zeroes the tid word on child exit. The
    // accompanying futex wake is not needed here because a HYPERION parent reaps
    // with wait4(2); the store is kept so a guest that polls the word sees the
    // value a real kernel would have left.
    if ((spec.flags & clone_flags::kChildCleartid) != 0 && spec.child_tidptr != 0) {
        const std::array<std::uint8_t, 4> zero{};
        (void)space.write(spec.child_tidptr, zero.data(), zero.size());
    }

    int status = 0;
    if (result.exited_via_syscall) {
        status = static_cast<int>(result.exit_code & 0xFF);
    } else if (result.status != RunStatus::kOk) {
        // The guest faulted or hit an unsupported instruction, so it never chose
        // an exit status. 127 is the shell convention for "could not run" and is
        // distinguishable from any status the guest picked for itself.
        status = 127;
    }

    // Flush before terminating: guest writes go through the host stdio sink and
    // _exit() does not run atexit handlers.
    std::fflush(stdout);
    std::fflush(stderr);
    // Closing the write end is what releases a parent blocked in vfork. On the
    // exec path this descriptor is O_CLOEXEC, so the exec closes it instead.
    if (vfork_write_fd >= 0) {
        ::close(vfork_write_fd);
    }
    ::_exit(status);
}

// --- parent side -------------------------------------------------------------

namespace {

// fork(2), vfork(2) and clone(2) share one implementation; only the clone spec
// and whether the parent suspends differ.
std::int64_t do_fork_common(MinimalSyscallHandler& self, const GuestState& state,
                            const ChildCloneSpec& spec, bool vfork_semantics) {
    GuestAddressSpace* space = ProcessAccess::address_space(self);
    if (space == nullptr) {
        // With no address space there is no guest memory to hand a child, so
        // there is no honest way to create one that could execute.
        return -kEnosys;
    }

    // Flush first: the child inherits the host stdio buffers, and without this
    // anything the guest printed before forking would be emitted twice.
    std::fflush(stdout);
    std::fflush(stderr);
    mark_owned_fds_cloexec(ProcessAccess::files(self));

    const GuestState child_state = state; // rip already points past `syscall`
    const std::vector<GuestDescriptor> child_files =
        fork_descriptor_table(ProcessAccess::files(self));
    const std::string sysroot = ProcessAccess::sysroot(self);
    const int tier = self.child_tier();
    const std::uint64_t max_steps = self.child_max_steps();

    int sync_fds[2] = {-1, -1};
    if (vfork_semantics) {
        if (::pipe2(sync_fds, O_CLOEXEC) != 0) {
            return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
        }
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int saved = errno;
        if (sync_fds[0] >= 0) {
            ::close(sync_fds[0]);
            ::close(sync_fds[1]);
        }
        // EAGAIN is the kernel's answer for resource exhaustion. Anything else
        // -- notably Android seccomp refusing the host clone -- is reported
        // verbatim so the guest sees the real reason rather than a guess.
        return -static_cast<std::int64_t>(saved == 0 ? kEagain : saved);
    }

    if (pid == 0) {
        // Child: a separate host process running a private copy of the guest.
        if (sync_fds[0] >= 0) {
            ::close(sync_fds[0]); // the read end belongs to the parent alone
        }
        // fork() returns 0 in the child. This has to be set here rather than by
        // the caller's `rax = r` assignment: the child never returns from the
        // syscall dispatch, it jumps straight into guest execution. Without it
        // the child would see the syscall *number* still sitting in rax (56 for
        // clone), take the parent's branch and wait on a pid that is not its
        // child. This was found by test, not by inspection.
        GuestState child = child_state;
        child.gpr[static_cast<std::size_t>(Gpr::kRax)] = 0;
        run_child_prologue(child, *space, child_files, sysroot, tier, max_steps, spec, sync_fds[1]);
    }

    // Parent. Discard the parent's translated code before it resumes: every
    // block in the cache was produced before the process split, and returning
    // to it after fork(2) has been observed to fault intermittently on an
    // AArch64 host. Bumping the epoch makes the translation cache drop its
    // blocks and re-translate, which costs one pass over the hot blocks and
    // removes the stale-translation window entirely.
    if (GuestAddressSpace* parent_as = ProcessAccess::address_space(self)) {
        parent_as->bump_code_epoch();
    }
    if (sync_fds[1] >= 0) {
        ::close(sync_fds[1]);
    }
    if (vfork_semantics && sync_fds[0] >= 0) {
        // Real vfork suspends the parent until the child execs or exits. The
        // child's write end is O_CLOEXEC so an execve closes it, and the child's
        // exit path closes it explicitly; either way the read returns EOF and the
        // parent resumes. Unlike the kernel this does not also share the address
        // space, which is a deliberate divergence documented in COMPATIBILITY.md.
        std::uint8_t drained[16];
        while (::read(sync_fds[0], drained, sizeof(drained)) > 0) {
        }
        ::close(sync_fds[0]);
    }

    ChildProcess child;
    child.guest_pid = static_cast<int>(pid);
    ProcessAccess::children(self).push_back(child);
    return static_cast<std::int64_t>(pid);
}

} // namespace

std::int64_t do_fork(MinimalSyscallHandler& self, GuestState& state, Memory& /*memory*/) {
    return do_fork_common(self, state, ChildCloneSpec{}, /*vfork_semantics=*/false);
}

std::int64_t do_vfork(MinimalSyscallHandler& self, GuestState& state, Memory& /*memory*/) {
    return do_fork_common(self, state, ChildCloneSpec{}, /*vfork_semantics=*/true);
}

std::int64_t do_clone(MinimalSyscallHandler& self, GuestState& state, Memory& memory) {
    const std::uint64_t flags = proc_arg(state, 0);
    // x86-64 clone argument order is (flags, child_stack, parent_tidptr,
    // child_tidptr, tls). AArch64 swaps the two tidptr arguments; this is the
    // *guest* ABI, so the x86-64 order is the only correct one here.
    const std::uint64_t parent_tidptr = proc_arg(state, 2);
    const std::uint64_t child_tidptr = proc_arg(state, 3);
    const std::uint64_t tls = proc_arg(state, 4);

    const std::uint64_t exit_signal = flags & clone_flags::kExitSignalMask;
    const std::uint64_t named_flags = flags & ~clone_flags::kExitSignalMask;

    if ((named_flags & kUnsupportedCloneFlags) != 0) {
        return -kEnosys;
    }
    if ((named_flags & ~kSupportedCloneFlags) != 0) {
        // An unrecognised flag is refused rather than ignored: silently dropping
        // a flag the guest relied on corrupts state the guest cannot detect.
        return -kEnosys;
    }
    // Only SIGCHLD is a meaningful exit signal here. An exit signal of 0 is what
    // a thread-creating clone uses (the child becomes unreapable), and is
    // accepted only when no other thread semantics were requested -- which the
    // checks above have already guaranteed is not the case.
    if (exit_signal != 0 && exit_signal != kSigchld) {
        return -kEnosys;
    }
    if ((named_flags & clone_flags::kSetTid) != 0 && tls == 0) {
        return -kEinval; // the kernel rejects CLONE_SETTLS with a null tls
    }
    // CLONE_PARENT_SETTID writes into the parent's memory before clone returns,
    // so a bad pointer must be caught before the child exists.
    if ((named_flags & clone_flags::kParentSetTid) != 0 && parent_tidptr != 0) {
        const std::array<std::uint8_t, 4> probe{};
        if (!memory.write(parent_tidptr, probe.data(), probe.size())) {
            return -kEfault;
        }
    }

    ChildCloneSpec spec;
    spec.flags = named_flags;
    spec.child_tidptr = child_tidptr;
    spec.tls = tls;

    const std::int64_t rc =
        do_fork_common(self, state, spec, (named_flags & clone_flags::kVfork) != 0);
    if (rc <= 0) {
        return rc;
    }

    if ((named_flags & clone_flags::kParentSetTid) != 0 && parent_tidptr != 0) {
        std::array<std::uint8_t, 4> raw{};
        put_le(raw.data(), 0, static_cast<std::uint64_t>(rc), 4);
        if (!memory.write(parent_tidptr, raw.data(), raw.size())) {
            return -kEfault;
        }
    }
    return rc;
}

std::int64_t do_wait4(MinimalSyscallHandler& self, GuestState& state, Memory& memory) {
    const std::int32_t pid = proc_arg_i32(state, 0);
    const std::uint64_t status_addr = proc_arg(state, 1);
    const std::int32_t options = proc_arg_i32(state, 2);
    const std::uint64_t rusage_addr = proc_arg(state, 3);

    constexpr std::int32_t kSupportedOptions =
        wait_options::kNoHang | wait_options::kUntraced | wait_options::kContinued;
    if ((options & ~kSupportedOptions) != 0) {
        // Refused rather than approximated: claiming WUNTRACED semantics while
        // stopped children are not tracked would be a lie the guest could act on.
        return -kEinval;
    }

    int status = 0;
    struct rusage usage{};
    const pid_t rc =
        ::wait4(static_cast<pid_t>(pid), &status, options, (rusage_addr != 0) ? &usage : nullptr);
    if (rc < 0) {
        // ECHILD is correct both for "no children at all" and for a pid that is
        // not this guest's child, which is what the kernel does.
        return -static_cast<std::int64_t>(errno == 0 ? kEchild : errno);
    }
    if (rc == 0) {
        return 0; // WNOHANG and no child has changed state yet
    }

    if (status_addr != 0) {
        // The host's wait status word is already in the encoding the guest
        // expects: WIFEXITED/WEXITSTATUS/WIFSIGNALED are defined identically in
        // terms of that word on both ABIs, so it is forwarded as 32 bits.
        std::array<std::uint8_t, 4> raw{};
        put_le(raw.data(), 0, static_cast<std::uint64_t>(status), 4);
        if (!memory.write(status_addr, raw.data(), raw.size())) {
            return -kEfault;
        }
    }

    if (rusage_addr != 0) {
        std::array<std::uint8_t, guest_rusage::kSize> raw{};
        put_le(raw.data(), 0, static_cast<std::uint64_t>(usage.ru_utime.tv_sec), 8);
        put_le(raw.data(), 8, static_cast<std::uint64_t>(usage.ru_utime.tv_usec), 8);
        put_le(raw.data(), 16, static_cast<std::uint64_t>(usage.ru_stime.tv_sec), 8);
        put_le(raw.data(), 24, static_cast<std::uint64_t>(usage.ru_stime.tv_usec), 8);
        put_le(raw.data(), guest_rusage::kMaxRss, static_cast<std::uint64_t>(usage.ru_maxrss), 8);
        put_le(raw.data(), guest_rusage::kMinFlt, static_cast<std::uint64_t>(usage.ru_minflt), 8);
        put_le(raw.data(), guest_rusage::kMajFlt, static_cast<std::uint64_t>(usage.ru_majflt), 8);
        put_le(raw.data(), guest_rusage::kInBlock, static_cast<std::uint64_t>(usage.ru_inblock), 8);
        put_le(raw.data(), guest_rusage::kOuBlock, static_cast<std::uint64_t>(usage.ru_oublock), 8);
        put_le(raw.data(), guest_rusage::kNvCs, static_cast<std::uint64_t>(usage.ru_nvcsw), 8);
        put_le(raw.data(), guest_rusage::kNivCs, static_cast<std::uint64_t>(usage.ru_nivcsw), 8);
        if (!memory.write(rusage_addr, raw.data(), raw.size())) {
            return -kEfault;
        }
    }

    // Record the reap, so a guest that waits for the same child twice gets
    // ECHILD the second time, exactly as it would from a real kernel.
    for (ChildProcess& c : ProcessAccess::children(self)) {
        if (c.guest_pid == static_cast<int>(rc)) {
            c.reaped = true;
            c.raw_status = status;
        }
    }
    return static_cast<std::int64_t>(rc);
}

std::int64_t do_execve(MinimalSyscallHandler& self, GuestState& state, Memory& memory) {
    GuestAddressSpace* space = ProcessAccess::address_space(self);
    if (space == nullptr) {
        return -kEnosys;
    }

    std::string name;
    std::int64_t rc = read_guest_string(memory, proc_arg(state, 0), name);
    if (rc != 0) {
        return rc;
    }
    if (name.empty()) {
        return -kEnoent;
    }

    std::vector<std::string> argv;
    rc = read_guest_string_array(memory, proc_arg(state, 1), argv);
    if (rc != 0) {
        return rc;
    }
    std::vector<std::string> envp;
    rc = read_guest_string_array(memory, proc_arg(state, 2), envp);
    if (rc != 0) {
        return rc;
    }

    // Resolve and read the new image *before* touching guest-visible state. A
    // real execve leaves the calling process untouched on failure, and a
    // translator that had already discarded the old image could not honour that;
    // validating first keeps every error path below non-destructive.
    std::string resolved;
    std::vector<std::uint8_t> bytes;
    rc = resolve_exec_path(name, ProcessAccess::sysroot(self), self.guest_image_path(),
                           self.guest_pid(), resolved, bytes);
    if (rc != 0) {
        return rc;
    }
    const ElfInfo info = inspect_elf(bytes.data(), bytes.size());
    if (!info.ok()) {
        // ENOEXEC is what the kernel returns for a file that is not a valid
        // executable. It is also what makes a shell fall back to interpreting the
        // file as a script, so getting it right changes behaviour.
        return -8; // -ENOEXEC
    }

    // Point of no return: from here the outgoing image is discarded.
    std::fflush(stdout);
    std::fflush(stderr);

    close_cloexec_descriptors(ProcessAccess::files(self));
    space->reset();

    // A fresh architectural state: execve discards registers, flags, the FPU
    // state and the TLS base, keeping only the descriptor table, credentials
    // and signal dispositions.
    GuestState fresh{};
    fresh.fpu_cw = 0x037F; // x87 control-word reset value
    fresh.mxcsr = 0x1F80;  // MXCSR reset value
    // `resolved` is the path this image was actually opened from, which is what
    // AT_EXECFN must contain; passing argv[0] instead would break a glibc guest
    // whose argv[0] is a bare name.
    const LoadedElf loaded =
        load_linux_elf_process(bytes.data(), bytes.size(), *space, fresh, argv, envp,
                               ProcessAccess::sysroot(self), 8ULL << 20, resolved);
    if (!loaded.ok()) {
        // The old image is already gone, so there is no process to return to.
        // Terminating is the least-surprising outcome, and it is reported loudly
        // rather than pretending the exec succeeded.
        std::fprintf(stderr, "hyperion: execve: cannot load '%s': %s\n", resolved.c_str(),
                     to_string(loaded.error));
        std::fflush(stderr);
        ::_exit(126);
    }

    state = fresh;
    if (syscall_trace_enabled()) {
        // write(2), not stdio: this must not interleave with or be swallowed by
        // the guest's own buffered output.
        char buf[512];
        const int n = std::snprintf(
            buf, sizeof(buf), "hyperion[pid %ld]: execve '%s' -> '%s' argc=%zu\n",
            static_cast<long>(::getpid()), name.c_str(), resolved.c_str(), argv.size());
        if (n > 0) {
            ssize_t unused = ::write(STDERR_FILENO, buf, static_cast<std::size_t>(n));
            (void)unused;
        }
    }
    ProcessAccess::guest_image_path(self) = resolved;
    ProcessAccess::guest_argv0(self) = argv.empty() ? resolved : argv.front();
    // reset() and the image mapping already bumped the address space's epoch, so
    // both execution tiers invalidate their translations of the outgoing image
    // before executing the first instruction of the new one.
    return 0;
}

} // namespace process
} // namespace hyperion
