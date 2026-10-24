// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Layered guest-syscall dispatch engine.
//
// The dispatcher is the single entry point for translating an x86-64 syscall
// number into host work. It is deliberately *additive* to the pre-existing
// handler in src/core/syscall.cpp: the existing `case sysno::kX:` arms remain the
// authority for every syscall they already implement, and the dispatcher only
// intercepts numbers that layer does not handle. Nothing about the pre-existing
// behaviour changes for a guest that never touches the registry.
//
// Dispatch order for one guest syscall (first match wins):
//
//   layer 0  bounds check          number >= kMaxSyscall -> unsupported path
//   layer 1  compiled table hit     constant-time index; a statically compiled
//                                   compatibility adapter, or a compiled
//                                   registry rule, is invoked directly
//   layer 2  existing handler       MinimalSyscallHandler::handle_unguarded(),
//                                   i.e. everything src/core/syscall.cpp already
//                                   implemented (with or without --linux-compat)
//   layer 3  unsupported            rax = -ENOSYS, one diagnostic recorded
//
// The table is a plain array indexed by syscall number, so layers 0 and 1 are a
// compare, a load and an indirect call. There is no hashing, no string
// comparison, no allocation, no lock and no file access on this path; the
// registry is parsed and compiled into the table once, at load time.
//
// Registry rules can never *replace* a statically implemented handler: a rule
// whose syscall number is already implemented by layer 2 is rejected at load
// time with a diagnostic. That is a safety property, not an oversight -- a
// writable JSON file must not be able to weaken a tested implementation.
#ifndef HYPERION_SYSCALL_DISPATCH_HPP
#define HYPERION_SYSCALL_DISPATCH_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/syscall.hpp"
#include "hyperion/syscall_registry.hpp"

namespace hyperion {

class MinimalSyscallHandler;
struct SyscallContext;

// A compatibility adapter: a compiled, whitelisted implementation of one guest
// syscall. It returns the value the guest must see in rax: a non-negative
// success value or a negative errno. Returning -ENOSYS means "this adapter
// cannot translate the request in the current configuration", which is counted
// as an unsupported syscall (never as a silent success).
using AdapterFn = std::int64_t (*)(SyscallContext&);

// Per-adapter capability flags.
enum AdapterFlags : std::uint32_t {
    kAdapterNone = 0,
    // The adapter requires the Box64-style Linux compatibility mode
    // (MinimalSyscallHandler::enable_linux_compat) to be enabled. When it is
    // not, the adapter returns -ENOSYS and the call is reported as unsupported.
    kAdapterNeedsLinuxCompat = 1U << 0,
};

// One entry of the compiled-in adapter whitelist. The registry selects a handler
// by `id`; nothing else in the registry can name executable behaviour.
struct AdapterDescriptor {
    const char* id;           // stable registry handler id, e.g. "compat.chdir"
    std::uint64_t syscall;    // x86-64 guest syscall number it implements
    const char* name;         // symbolic name for diagnostics
    AdapterFn fn;             // compiled implementation
    std::uint32_t flags;      // AdapterFlags
    const char* summary;      // one-line description for `hyperion syscall-registry list`
    // Null-terminated list of ABI transformations this adapter performs. A
    // registry rule must declare exactly this set, so a rule can never claim a
    // transformation that is not performed nor omit one that is.
    const char* const* transforms;
};

// The compiled-in whitelist. Sorted by syscall number.
const AdapterDescriptor* builtin_adapters(std::size_t& count);
const AdapterDescriptor* find_builtin_adapter(std::string_view id);
const AdapterDescriptor* find_builtin_adapter_by_syscall(std::uint64_t number);

// Fast-path table geometry. The x86-64 syscall space used by real programs is
// well below 512 (the largest number HYPERION documents is 451); numbers at or
// above this bound take the unsupported path, which is still correct (-ENOSYS).
inline constexpr std::size_t kMaxSyscall = 512;

// What a table slot does.
enum class DispatchKind : std::uint8_t {
    kNone = 0,       // no compiled entry: fall through to the existing handler
    kAdapter = 1,    // call `fn`
    kGuestAlias = 2, // rewrite argument registers and re-dispatch the target
};

struct DispatchEntry {
    AdapterFn fn = nullptr;
    DispatchKind kind = DispatchKind::kNone;
    std::uint8_t reserved = 0;
    std::uint32_t flags = 0;
    const char* name = nullptr; // symbolic name, or nullptr
};

// A compiled alias rule: argument i of the *target* syscall comes from either a
// constant or from argument `value` of the *source* syscall.
struct CompiledAlias {
    std::uint64_t target = 0;
    std::array<std::uint8_t, 6> from_arg{};   // 0xFF = not from an argument
    std::array<std::int64_t, 6> constant{};
    std::uint8_t count = 0;
};

// One immutable, fully validated snapshot of compiled rules. A dispatcher holds
// a shared_ptr to it, so a reload can build a replacement and swap it in without
// invalidating a concurrent reader's view.
struct CompiledRules {
    std::array<DispatchEntry, kMaxSyscall> table{};
    std::vector<CompiledAlias> aliases;  // index referenced by DispatchKind::kGuestAlias
    std::vector<RegistryRule> rules;
    std::string source_path;
    std::string fingerprint;
    std::uint64_t adapter_rules = 0;
    std::uint64_t alias_rules = 0;
};

// Aggregate diagnostics. Counters are relaxed atomics so a guest thread and a
// reporting thread cannot race; a relaxed increment is a single store-class
// instruction and is *not* on the success path of an unsupported call only.
struct SyscallCounters {
    std::atomic<std::uint64_t> handled_adapter{0};
    std::atomic<std::uint64_t> handled_alias{0};
    std::atomic<std::uint64_t> handled_existing{0};
    std::atomic<std::uint64_t> unsupported_events{0};
    std::atomic<std::uint64_t> translation_failures{0};
    std::atomic<std::uint64_t> registry_errors{0};
    std::atomic<std::uint64_t> registry_rejected{0};

    void reset() noexcept;
};

// The set of syscall numbers seen on the unsupported path, plus their event
// counts. Bounded and allocation-free: the numbers are packed into a bitmap for
// 0..kMaxSyscall and a small overflow list for anything larger.
class UnsupportedSet {
public:
    static constexpr std::size_t kMaxTracked = kMaxSyscall;
    static constexpr std::size_t kOverflowSlots = 16;

    void clear() noexcept;
    void add(std::uint64_t number) noexcept;

    bool contains(std::uint64_t number) const noexcept;
    std::size_t unique_count() const noexcept;
    // Writes the tracked numbers in ascending order (then overflow numbers in
    // first-seen order). Returns how many were written.
    std::size_t snapshot(std::uint64_t* out, std::size_t capacity) const noexcept;

private:
    std::array<std::uint64_t, (kMaxTracked + 63) / 64> bits_{};
    std::array<std::uint64_t, kOverflowSlots> overflow_{};
    std::size_t overflow_count_ = 0;
    std::size_t count_ = 0;
};

// Guest-visible view of one syscall, handed to an adapter.
//
// Adapters never see a raw guest pointer: they receive this context, which
// validates every guest address through `Memory`. The context is a stack
// object; it owns nothing and its lifetime is a single dispatch.
struct SyscallContext {
    MinimalSyscallHandler& handler;
    GuestState& state;
    Memory& memory;
    std::uint64_t number;

    std::uint64_t arg(int index) const;
    std::int32_t arg_i32(int index) const;

    // Negative errno helper so adapters read `return ctx.fail(kEnosys);`.
    static constexpr std::int64_t fail(std::int64_t errno_value) noexcept { return -errno_value; }

    // Linux compatibility mode is on (enable_linux_compat()).
    bool linux_compat() const;

    // sysroot prefix for absolute guest paths ("" when unset).
    const std::string& sysroot() const;

    // Guest fd -> host fd, or a negative errno (-EBADF) when unmapped. Adapters
    // must use this instead of assuming guest and host numbering agree.
    std::int64_t host_fd(std::int32_t guest_fd) const;

    // Install a host descriptor as a new guest descriptor. `fd` in / `cloexec`
    // follow the guest's own numbering rules.
    std::int64_t add_host_fd(int host_fd, bool cloexec);

    // Read a NUL-terminated guest string (PATH_MAX bounded).
    std::int64_t read_cstr(std::uint64_t address, std::string& out) const;

    // Apply the configured sysroot to an absolute guest path.
    std::string resolve_path(const std::string& guest_path) const;

    // Map an optional guest dirfd argument to a host dirfd (AT_FDCWD pass-through).
    std::int64_t host_dirfd(std::int32_t guest_dirfd) const;

    bool copy_in(std::uint64_t address, void* dst, std::size_t count) const;
    bool copy_out(std::uint64_t address, const void* src, std::size_t count) const;
};

// Narrow, audited access to the handler state an adapter needs. This mirrors the
// ProcessAccess pattern used by src/core/process.cpp: one friend struct lists
// exactly which internals the adapter layer may touch, instead of adding public
// accessors for every field.
struct SyscallAccess {
    static const std::vector<GuestDescriptor>& files(const MinimalSyscallHandler& handler) {
        return handler.files_;
    }
    static std::vector<GuestDescriptor>& mutable_files(MinimalSyscallHandler& handler) {
        return handler.files_;
    }
    static bool linux_compat(const MinimalSyscallHandler& handler) {
        return handler.linux_compat_;
    }
    static const std::string& sysroot(const MinimalSyscallHandler& handler) {
        return handler.sysroot_;
    }
};

// How the top-level handler integrates with the dispatcher.
//
// The dispatcher owns a shared, immutable compiled-rule snapshot. A
// MinimalSyscallHandler holds one dispatcher; fork(2)/clone(2) children build
// their own handler but share the same process-wide snapshot, so parsing a
// registry happens once per process, not once per child and never per syscall.
class SyscallDispatcher {
public:
    SyscallDispatcher();

    // Replace the compiled rules by (re)loading the registry. The new snapshot
    // is parsed and fully validated *before* it is swapped in; a failed load
    // leaves the previous snapshot in service and returns a non-ok report.
    RegistryReport reload(const RegistryOptions& options);

    // Parse errors / rejected rules from the most recent successful or failed
    // load, for diagnostics.
    const std::vector<std::string>& diagnostics() const noexcept { return diagnostics_; }
    const CompiledRules* rules() const noexcept { return rules_.get(); }
    const std::string& registry_path() const noexcept { return registry_path_; }

    // --- hot path ---------------------------------------------------------
    //
    // Returns the table slot for `number`, or nullptr when the existing handler
    // must be used. Inline and branch-predictable: callers test the kind.
    const DispatchEntry* lookup(std::uint64_t number) const noexcept {
        if (number >= kMaxSyscall) {
            return nullptr;
        }
        const DispatchEntry& e = rules_->table[static_cast<std::size_t>(number)];
        return e.kind == DispatchKind::kNone ? nullptr : &e;
    }

    void note_adapter_success() noexcept { counters_.handled_adapter.fetch_add(1, std::memory_order_relaxed); }
    void note_alias_success() noexcept { counters_.handled_alias.fetch_add(1, std::memory_order_relaxed); }
    void note_existing_success() noexcept { counters_.handled_existing.fetch_add(1, std::memory_order_relaxed); }
    void note_unsupported(std::uint64_t number) noexcept {
        counters_.unsupported_events.fetch_add(1, std::memory_order_relaxed);
        unsupported_.add(number);
    }
    void note_translation_failure() noexcept {
        counters_.translation_failures.fetch_add(1, std::memory_order_relaxed);
    }

    const SyscallCounters& counters() const noexcept { return counters_; }
    const UnsupportedSet& unsupported() const noexcept { return unsupported_; }

    // Human-readable summary of the aggregates. Multi-line, no trailing newline.
    std::string summary() const;

private:
    std::shared_ptr<const CompiledRules> rules_;
    SyscallCounters counters_;
    UnsupportedSet unsupported_;
    std::vector<std::string> diagnostics_;
    std::string registry_path_;
};

// Resolve the guest syscall number to its symbolic name for diagnostics.
// Returns nullptr when the number is unknown to the built-in table.
const char* guest_syscall_name(std::uint64_t number);

// True when `number` is implemented by a compiled-in compatibility adapter.
bool is_compat_adapter_syscall(std::uint64_t number);

} // namespace hyperion

#endif // HYPERION_SYSCALL_DISPATCH_HPP
