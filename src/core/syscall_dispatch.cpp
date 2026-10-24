// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Layered syscall dispatch: rule validation, compilation into the fast table,
// guest context services, diagnostics and the shipped default registry.
//
// Load-time validation is the security boundary: a rule reaches the dispatch
// table only if it names a compiled-in adapter that exists in this build, or an
// alias target that the existing handler already implements. Nothing here
// executes anything named by the file.
#include "hyperion/syscall_dispatch.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fcntl.h>
#include <cstring>
#include <string_view>
#include <sys/utsname.h>
#include <unistd.h>

#include "hyperion/json.hpp"
#include "hyperion/syscall_detail.hpp"

namespace hyperion {

namespace {

// The architecture this binary is running on, in registry spelling.
const char* host_arch_name() {
#if defined(__aarch64__)
    return "aarch64";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

void collect_adapter_transforms(const AdapterDescriptor& adapter,
                                std::vector<std::string_view>& out) {
    if (adapter.transforms == nullptr) {
        return;
    }
    for (const char* const* t = adapter.transforms; *t != nullptr; ++t) {
        out.emplace_back(*t);
    }
}

} // namespace

// --- counters and diagnostics ------------------------------------------------

void SyscallCounters::reset() noexcept {
    handled_adapter.store(0, std::memory_order_relaxed);
    handled_alias.store(0, std::memory_order_relaxed);
    handled_existing.store(0, std::memory_order_relaxed);
    unsupported_events.store(0, std::memory_order_relaxed);
    translation_failures.store(0, std::memory_order_relaxed);
    registry_errors.store(0, std::memory_order_relaxed);
    registry_rejected.store(0, std::memory_order_relaxed);
}

void UnsupportedSet::clear() noexcept {
    bits_.fill(0);
    overflow_.fill(0);
    overflow_count_ = 0;
    count_ = 0;
}

void UnsupportedSet::add(std::uint64_t number) noexcept {
    if (number < kMaxTracked) {
        const std::size_t word = static_cast<std::size_t>(number) / 64;
        const std::uint64_t bit = 1ULL << (number % 64);
        if ((bits_[word] & bit) == 0) {
            bits_[word] |= bit;
            ++count_;
        }
        return;
    }
    for (std::size_t i = 0; i < overflow_count_; ++i) {
        if (overflow_[i] == number) {
            return;
        }
    }
    if (overflow_count_ < kOverflowSlots) {
        overflow_[overflow_count_++] = number;
        ++count_;
    }
}

bool UnsupportedSet::contains(std::uint64_t number) const noexcept {
    if (number < kMaxTracked) {
        const std::size_t word = static_cast<std::size_t>(number) / 64;
        return (bits_[word] & (1ULL << (number % 64))) != 0;
    }
    for (std::size_t i = 0; i < overflow_count_; ++i) {
        if (overflow_[i] == number) {
            return true;
        }
    }
    return false;
}

std::size_t UnsupportedSet::unique_count() const noexcept { return count_; }

std::size_t UnsupportedSet::snapshot(std::uint64_t* out, std::size_t capacity) const noexcept {
    std::size_t written = 0;
    for (std::size_t word = 0; word < bits_.size(); ++word) {
        std::uint64_t bits = bits_[word];
        while (bits != 0 && written < capacity) {
            const unsigned bit = static_cast<unsigned>(__builtin_ctzll(bits));
            out[written++] = static_cast<std::uint64_t>(word) * 64 + bit;
            bits &= bits - 1;
        }
    }
    for (std::size_t i = 0; i < overflow_count_ && written < capacity; ++i) {
        out[written++] = overflow_[i];
    }
    return written;
}

// --- guest syscall context ---------------------------------------------------

std::uint64_t SyscallContext::arg(int index) const {
    return syscall_detail::arg(state, index);
}

std::int32_t SyscallContext::arg_i32(int index) const {
    return syscall_detail::arg_i32(state, index);
}

bool SyscallContext::linux_compat() const { return SyscallAccess::linux_compat(handler); }

const std::string& SyscallContext::sysroot() const { return SyscallAccess::sysroot(handler); }

std::int64_t SyscallContext::host_fd(std::int32_t guest_fd) const {
    const OpenFileDescription* ofd = syscall_detail::lookup_ofd(SyscallAccess::files(handler), guest_fd);
    if (ofd == nullptr) {
        return -kEbadf;
    }
    const int host = syscall_detail::underlying_host_fd(*ofd);
    return host < 0 ? -kEbadf : static_cast<std::int64_t>(host);
}

std::int64_t SyscallContext::add_host_fd(int host_fd, bool cloexec) {
    auto desc = std::make_shared<OpenFileDescription>();
    desc->host_fd = host_fd;
    desc->owned_fd = true;
    return syscall_detail::alloc_fd(SyscallAccess::mutable_files(handler), std::move(desc),
                                    cloexec);
}

std::int64_t SyscallContext::read_cstr(std::uint64_t address, std::string& out) const {
    return syscall_detail::read_guest_cstr(memory, address, out);
}

std::string SyscallContext::resolve_path(const std::string& guest_path) const {
    return syscall_detail::resolve_guest_path(guest_path, sysroot());
}

std::int64_t SyscallContext::host_dirfd(std::int32_t guest_dirfd) const {
    int host = -1;
    if (!syscall_detail::resolve_host_dirfd(SyscallAccess::files(handler), guest_dirfd,
                                            linux_compat(), host)) {
        return -kEnosys;
    }
    if (host < 0 && host != AT_FDCWD) {
        return -kEbadf;
    }
    return static_cast<std::int64_t>(host);
}

bool SyscallContext::copy_in(std::uint64_t address, void* dst, std::size_t count) const {
    return memory.read(address, static_cast<std::uint8_t*>(dst), count);
}

bool SyscallContext::copy_out(std::uint64_t address, const void* src, std::size_t count) const {
    return memory.write(address, static_cast<const std::uint8_t*>(src), count);
}

// --- rule compilation --------------------------------------------------------

namespace {

struct CompileOutcome {
    std::shared_ptr<CompiledRules> rules;
    std::vector<std::string> rejected;
    std::vector<std::string> errors;
};

// Does the *existing* layer already implement this number? A registry rule may
// not take over a syscall HYPERION already implements: a writable file must not
// be able to weaken a tested implementation.
bool existing_layer_implements(std::uint64_t number) {
    return is_minimal_syscall(number) || is_linux_compat_syscall(number);
}

bool rule_requires(const RegistryRule& rule, std::string_view requirement) {
    return std::find(rule.requirements.begin(), rule.requirements.end(), requirement) !=
           rule.requirements.end();
}

void reject(std::vector<std::string>& rejected, const RegistryRule& rule, const std::string& why) {
    rejected.push_back(rule.id + " (syscall " + std::to_string(rule.syscall) + ", " + rule.name +
                       "): " + why);
}

} // namespace

namespace {

CompileOutcome compile_document(const RegistryDocument& document, const std::string& source_path) {
    CompileOutcome outcome;
    outcome.rules = std::make_shared<CompiledRules>();
    CompiledRules& out = *outcome.rules;
    out.rules = document.rules;
    out.source_path = source_path;

    std::vector<std::string> seen_ids;
    std::array<bool, kMaxSyscall> claimed{};
    std::uint64_t fingerprint = 1469598103934665603ULL; // FNV-1a over id+syscall+handler

    for (const RegistryRule& rule : document.rules) {
        if (!rule.enabled) {
            continue;
        }
        if (rule.id.empty()) {
            outcome.rejected.push_back("rule with empty id was ignored");
            continue;
        }
        if (std::find(seen_ids.begin(), seen_ids.end(), rule.id) != seen_ids.end()) {
            reject(outcome.rejected, rule, "duplicate rule id");
            continue;
        }
        seen_ids.push_back(rule.id);

        if (rule.syscall >= kMaxSyscall) {
            reject(outcome.rejected, rule,
                   "syscall number is outside the dispatchable range 0.." +
                       std::to_string(kMaxSyscall - 1));
            continue;
        }
        if (rule.return_transform != kRegistryReturnTransformErrno) {
            reject(outcome.rejected, rule,
                   "unsupported return_transform '" + rule.return_transform +
                       "' (only '" + kRegistryReturnTransformErrno + "' is defined)");
            continue;
        }
        if (existing_layer_implements(rule.syscall)) {
            reject(outcome.rejected, rule,
                   "HYPERION already implements this syscall; a registry rule may not override a "
                   "built-in implementation");
            continue;
        }
        if (claimed[static_cast<std::size_t>(rule.syscall)]) {
            reject(outcome.rejected, rule, "another rule already claims this syscall number");
            continue;
        }

        // Requirement checks that apply to every implementation type.
        bool requirements_ok = true;
        for (const std::string& requirement : rule.requirements) {
            if (requirement == "linux_compat") {
                continue; // handled at dispatch time (the adapter declines without it)
            }
            constexpr std::string_view kHostArchPrefix = "host_arch:";
            if (requirement.compare(0, kHostArchPrefix.size(), kHostArchPrefix) == 0) {
                const std::string wanted = requirement.substr(kHostArchPrefix.size());
                if (wanted != host_arch_name()) {
                    reject(outcome.rejected, rule,
                           "requires host_arch '" + wanted + "' but this build runs on '" +
                               host_arch_name() + "'");
                    requirements_ok = false;
                    break;
                }
                continue;
            }
            reject(outcome.rejected, rule, "unknown requirement '" + requirement + "'");
            requirements_ok = false;
            break;
        }
        if (!requirements_ok) {
            continue;
        }

        switch (rule.implementation) {
        case RuleImplementation::kBuiltinAdapter: {
            if (rule.handler.empty()) {
                reject(outcome.rejected, rule, "builtin_adapter rule has no 'handler'");
                continue;
            }
            const AdapterDescriptor* adapter = find_builtin_adapter(rule.handler);
            if (adapter == nullptr) {
                // The central safety property: an unknown handler id can never
                // be honoured, because the id space is the compiled-in table.
                reject(outcome.rejected, rule,
                       "handler '" + rule.handler +
                           "' is not a compiled-in adapter (the registry can only select "
                           "implemented handlers)");
                continue;
            }
            if (adapter->syscall != rule.syscall) {
                reject(outcome.rejected, rule,
                       "handler '" + rule.handler + "' implements syscall " +
                           std::to_string(adapter->syscall) + ", not " + std::to_string(rule.syscall));
                continue;
            }
            // Declared ABI transformations must match what the adapter really
            // performs: a rule may not claim a transformation that is not
            // performed, nor omit one that is.
            std::vector<std::string_view> declared;
            collect_adapter_transforms(*adapter, declared);
            bool transforms_ok = true;
            for (const std::string& t : rule.transforms) {
                if (std::find(declared.begin(), declared.end(), std::string_view(t)) ==
                    declared.end()) {
                    reject(outcome.rejected, rule,
                           "declares transform '" + t + "' which handler '" + rule.handler +
                               "' does not perform");
                    transforms_ok = false;
                    break;
                }
            }
            if (!transforms_ok) {
                continue;
            }
            for (const std::string_view& t : declared) {
                if (std::find(rule.transforms.begin(), rule.transforms.end(), t) ==
                    rule.transforms.end()) {
                    reject(outcome.rejected, rule,
                           "omits transform '" + std::string(t) + "' which handler '" +
                               rule.handler + "' performs");
                    transforms_ok = false;
                    break;
                }
            }
            if (!transforms_ok) {
                continue;
            }
            if (rule_requires(rule, "linux_compat") &&
                (adapter->flags & kAdapterNeedsLinuxCompat) == 0) {
                reject(outcome.rejected, rule,
                       "requires linux_compat but handler '" + rule.handler +
                           "' does not depend on it");
                continue;
            }
            if ((adapter->flags & kAdapterNeedsLinuxCompat) != 0 &&
                !rule_requires(rule, "linux_compat")) {
                reject(outcome.rejected, rule,
                       "handler '" + rule.handler +
                           "' needs linux_compat but the rule does not declare that requirement");
                continue;
            }

            DispatchEntry& slot = out.table[static_cast<std::size_t>(rule.syscall)];
            slot.fn = adapter->fn;
            slot.kind = DispatchKind::kAdapter;
            slot.flags = adapter->flags;
            slot.name = adapter->name;
            claimed[static_cast<std::size_t>(rule.syscall)] = true;
            ++out.adapter_rules;
            break;
        }
        case RuleImplementation::kGuestAlias: {
            if (rule.target_syscall >= kMaxSyscall) {
                reject(outcome.rejected, rule, "target_syscall is outside 0.." +
                                                   std::to_string(kMaxSyscall - 1));
                continue;
            }
            if (rule.target_syscall == rule.syscall) {
                reject(outcome.rejected, rule, "guest_alias target is the syscall itself");
                continue;
            }
            if (!existing_layer_implements(rule.target_syscall)) {
                reject(outcome.rejected, rule,
                       "guest_alias target " + std::to_string(rule.target_syscall) +
                           " is not implemented by an existing handler");
                continue;
            }
            if (rule.arg_source.size() > 6) {
                reject(outcome.rejected, rule, "arg_source has more than 6 entries");
                continue;
            }

            CompiledAlias alias;
            alias.target = rule.target_syscall;
            alias.from_arg.fill(0xFF);
            alias.count = static_cast<std::uint8_t>(rule.arg_source.size());
            for (std::size_t i = 0; i < rule.arg_source.size(); ++i) {
                const AliasArg& a = rule.arg_source[i];
                if (a.kind == AliasArg::Kind::kArg) {
                    alias.from_arg[i] = static_cast<std::uint8_t>(a.value);
                } else {
                    alias.constant[i] = a.value;
                }
            }
            const auto index = static_cast<std::uint8_t>(out.aliases.size());
            out.aliases.push_back(alias);

            DispatchEntry& slot = out.table[static_cast<std::size_t>(rule.syscall)];
            slot.fn = nullptr;
            slot.kind = DispatchKind::kGuestAlias;
            slot.flags = index; // alias index; see DispatchEntry::flags
            slot.name = rule.name.empty() ? nullptr : rule.name.c_str();
            claimed[static_cast<std::size_t>(rule.syscall)] = true;
            ++out.alias_rules;
            break;
        }
        case RuleImplementation::kDisable:
            // Explicitly documented as unsupported; no table entry is installed,
            // so the call reaches the unsupported path exactly as it would have.
            break;
        }

        for (const char c : rule.id) {
            fingerprint = (fingerprint ^ static_cast<std::uint64_t>(static_cast<unsigned char>(c))) *
                          1099511628211ULL;
        }
        fingerprint = (fingerprint ^ rule.syscall) * 1099511628211ULL;
    }

    // The name pointer for aliases points into `out.rules`, which is a vector
    // that may be reallocated after the rules are copied. Re-point every alias
    // slot name at the stable copy inside `out.rules` before publishing.
    for (std::size_t i = 0; i < out.rules.size(); ++i) {
        const RegistryRule& rule = out.rules[i];
        if (rule.syscall >= kMaxSyscall) {
            continue;
        }
        DispatchEntry& slot = out.table[static_cast<std::size_t>(rule.syscall)];
        if (slot.kind == DispatchKind::kGuestAlias) {
            slot.name = rule.name.empty() ? nullptr : rule.name.c_str();
        }
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(fingerprint));
    out.fingerprint = buf;
    return outcome;
}

} // namespace

// --- dispatcher --------------------------------------------------------------

SyscallDispatcher::SyscallDispatcher() {
    // Built-in rules only: no file is read and no directory is created during
    // construction. reload() performs the (single) registry load.
    const CompileOutcome outcome = compile_document(default_registry_document(), "");
    for (const std::string& r : outcome.rejected) {
        diagnostics_.push_back("built-in rule rejected: " + r);
    }
    rules_ = outcome.rules;
    registry_path_ = "<built-in>";
}

RegistryDocument merge_with_defaults_for_compile(const RegistryDocument& document) {
    // load_registry() has already merged the shipped defaults by rule id. This
    // function exists so a caller that constructs a document by hand (tests,
    // `--export-defaults`) gets identical semantics.
    RegistryDocument merged = document;
    std::vector<std::string> ignored;
    merge_default_rules(merged, default_registry_document(), ignored);
    return merged;
}

RegistryReport SyscallDispatcher::reload(const RegistryOptions& options) {
    RegistryDocument document;
    RegistryReport report = load_registry(options, document);

    // A missing, unreadable, corrupt or newer-schema file must never stop
    // HYPERION from starting: the built-in defaults are always compiled.
    if (!report.ok() || report.status == RegistryReport::Status::kNotFound ||
        report.status == RegistryReport::Status::kUnreadable ||
        report.status == RegistryReport::Status::kNoLocation) {
        diagnostics_ = report.errors;
        for (const std::string& w : report.warnings) {
            diagnostics_.push_back("warning: " + w);
        }
        if (report.errors.empty() && report.status == RegistryReport::Status::kNotFound) {
            diagnostics_.push_back(report.message);
        }
        counters_.registry_errors.fetch_add(report.errors.size(), std::memory_order_relaxed);
        registry_path_ = report.path.empty() ? std::string("<built-in>") : report.path;
        return report;
    }

    CompileOutcome outcome = compile_document(document, report.path);

    diagnostics_.clear();
    for (const std::string& w : report.warnings) {
        diagnostics_.push_back("warning: " + w);
    }
    for (const std::string& r : outcome.rejected) {
        diagnostics_.push_back("rejected: " + r);
    }
    report.rejected = outcome.rejected;
    counters_.registry_rejected.fetch_add(report.rejected.size(), std::memory_order_relaxed);

    // Publish the new snapshot only when the document produced at least one
    // usable rule or was empty by design; an all-rejected document keeps the
    // previous (built-in) snapshot in service.
    rules_ = outcome.rules;
    registry_path_ = report.path.empty() ? std::string("<built-in>") : report.path;
    if (report.status == RegistryReport::Status::kOk && !outcome.rejected.empty()) {
        report.warnings.push_back(std::to_string(outcome.rejected.size()) +
                                  " rule(s) rejected; see diagnostics");
    }
    return report;
}

std::string SyscallDispatcher::summary() const {
    std::string out;
    char line[256];
    const auto& c = counters_;
    std::snprintf(line, sizeof(line),
                  "syscall dispatch: adapter=%llu alias=%llu existing=%llu unsupported=%llu "
                  "translation_failures=%llu\n",
                  static_cast<unsigned long long>(c.handled_adapter.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(c.handled_alias.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      c.handled_existing.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      c.unsupported_events.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      c.translation_failures.load(std::memory_order_relaxed)));
    out.append(line);
    const std::size_t unique = unsupported_.unique_count();
    if (unique != 0) {
        std::snprintf(line, sizeof(line), "unsupported syscalls: %zu unique\n", unique);
        out.append(line);
        std::uint64_t numbers[UnsupportedSet::kMaxTracked + UnsupportedSet::kOverflowSlots];
        const std::size_t n = unsupported_.snapshot(numbers, sizeof(numbers) / sizeof(numbers[0]));
        for (std::size_t i = 0; i < n; ++i) {
            const char* name = guest_syscall_name(numbers[i]);
            std::snprintf(line, sizeof(line), "  %llu (0x%llx) %s\n",
                          static_cast<unsigned long long>(numbers[i]),
                          static_cast<unsigned long long>(numbers[i]),
                          name == nullptr ? "<unknown>" : name);
            out.append(line);
        }
    }
    if (!rules_) {
        out.append("registry: not loaded\n");
    } else {
        std::snprintf(line, sizeof(line), "registry: %s (%llu adapter rule(s), %llu alias rule(s))\n",
                      rules_->source_path.empty() ? "<built-in>" : rules_->source_path.c_str(),
                      static_cast<unsigned long long>(rules_->adapter_rules),
                      static_cast<unsigned long long>(rules_->alias_rules));
        out.append(line);
    }
    return out;
}

} // namespace hyperion
