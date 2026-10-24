// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Tests for the layered dispatch engine: the compiled table, registry rule
// validation (including every rejection path that keeps a JSON file from
// claiming an implementation that does not exist), and the diagnostics.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

#include "harness/test_harness.hpp"
#include "harness/test_tempdir.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/syscall.hpp"
#include "hyperion/syscall_detail.hpp"
#include "hyperion/syscall_dispatch.hpp"
#include "hyperion/syscall_registry.hpp"

using hyperion::DispatchKind;
using hyperion::Gpr;
using hyperion::GuestState;
using hyperion::MinimalSyscallHandler;
using hyperion::RegistryOptions;
using hyperion::RegistryReport;
using hyperion::RegistryRule;
using hyperion::RuleImplementation;
using hyperion::RuleOrigin;
using hyperion::SyscallStatus;
using hyperion::test::TempDir;

namespace {

constexpr std::uint64_t kBase = 0x400000;
constexpr std::size_t kMemSize = 4096;

// Write `text` to `path` verbatim.
void write_text(const std::string& path, const std::string& text) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    HYPERION_CHECK(f != nullptr);
    HYPERION_CHECK_EQ(std::fwrite(text.data(), 1, text.size(), f), text.size());
    std::fclose(f);
}

// A registry document containing exactly one rule, serialised by the real
// writer so the test exercises the same JSON the shipped defaults use.
std::string document_with_rule(const RegistryRule& rule) {
    hyperion::RegistryDocument document;
    document.rules.push_back(rule);
    return hyperion::document_to_text(document);
}

RegistryRule chdir_rule() {
    RegistryRule rule;
    rule.id = "user.chdir";
    rule.name = "chdir";
    rule.syscall = 80;
    rule.implementation = hyperion::RuleImplementation::kBuiltinAdapter;
    rule.handler = "compat.chdir";
    rule.transforms = {"path.sysroot"};
    rule.origin = hyperion::RuleOrigin::kUser;
    rule.enabled = true;
    return rule;
}

RegistryOptions options_at(const std::string& path) {
    RegistryOptions options;
    options.use_environment = false;
    options.explicit_path = path;
    // Do not create a file: the test wrote one already.
    return options;
}

} // namespace

// --- the compiled-in table ---------------------------------------------------

HYPERION_TEST("dispatch", "builtin_defaults_compile_into_the_table") {
    MinimalSyscallHandler handler({}, nullptr);
    const hyperion::SyscallDispatcher& dispatcher = handler.dispatcher();
    HYPERION_CHECK(dispatcher.rules() != nullptr);
    // The shipped defaults are generated from the adapter table, so every
    // adapter must be reachable and no rule may be rejected.
    std::size_t adapter_count = 0;
    (void)hyperion::builtin_adapters(adapter_count);
    HYPERION_CHECK_EQ(dispatcher.rules()->adapter_rules, adapter_count);
    HYPERION_CHECK_EQ(dispatcher.rules()->alias_rules, std::uint64_t{2});
    HYPERION_CHECK(dispatcher.diagnostics().empty());
}

HYPERION_TEST("dispatch", "every_adapter_has_a_clean_lookup") {
    MinimalSyscallHandler handler({}, nullptr);
    std::size_t count = 0;
    const hyperion::AdapterDescriptor* adapters = hyperion::builtin_adapters(count);
    for (std::size_t i = 0; i < count; ++i) {
        const hyperion::DispatchEntry* entry = handler.dispatcher().lookup(adapters[i].syscall);
        HYPERION_CHECK(entry != nullptr);
        HYPERION_CHECK(entry->kind == DispatchKind::kAdapter);
        HYPERION_CHECK(entry->fn != nullptr);
        HYPERION_CHECK_EQ(std::string(entry->name), std::string(adapters[i].name));
    }
}

HYPERION_TEST("dispatch", "existing_handlers_are_not_shadowed") {
    MinimalSyscallHandler handler({}, nullptr);
    // write(2), read(2) and openat(2) are implemented by the existing switch and
    // must not appear in the compatibility table: the registry may never take
    // over a built-in implementation.
    HYPERION_CHECK(handler.dispatcher().lookup(1) == nullptr);
    HYPERION_CHECK(handler.dispatcher().lookup(0) == nullptr);
    HYPERION_CHECK(handler.dispatcher().lookup(257) == nullptr);
    // Out-of-range numbers have no entry and are handled by the unsupported path.
    HYPERION_CHECK(handler.dispatcher().lookup(hyperion::kMaxSyscall) == nullptr);
    HYPERION_CHECK(handler.dispatcher().lookup(0xFFFFFFFFULL) == nullptr);
}

// --- validation: a rule may not invent an implementation ---------------------

HYPERION_TEST("dispatch", "unknown_handler_id_is_rejected") {
    TempDir dir;
    RegistryRule rule = chdir_rule();
    rule.handler = "compat.does_not_exist";
    write_text(dir.file("registry.json"), document_with_rule(rule));

    MinimalSyscallHandler handler({}, nullptr);
    std::string message;
    HYPERION_CHECK_EQ(
        handler.reload_syscall_registry(options_at(dir.file("registry.json")), message), 0);
    const auto& diagnostics = handler.dispatcher().diagnostics();
    bool found = false;
    for (const std::string& d : diagnostics) {
        found = found || d.find("is not a compiled-in adapter") != std::string::npos;
    }
    HYPERION_CHECK(found);
    // The rule is rejected, so no adapter is installed for 80 by this rule.
    HYPERION_CHECK(handler.dispatcher().lookup(80) != nullptr); // the *default* rule is
    HYPERION_CHECK(handler.dispatcher().rules()->adapter_rules > 0);
}

HYPERION_TEST("dispatch", "handler_for_a_different_syscall_is_rejected") {
    TempDir dir;
    RegistryRule rule = chdir_rule();
    rule.syscall = 300; // handler compat.chdir implements 80, not 300
    write_text(dir.file("registry.json"), document_with_rule(rule));

    MinimalSyscallHandler handler({}, nullptr);
    std::string message;
    (void)handler.reload_syscall_registry(options_at(dir.file("registry.json")), message);
    bool found = false;
    for (const std::string& d : handler.dispatcher().diagnostics()) {
        found = found || d.find("implements syscall 80, not 300") != std::string::npos;
    }
    HYPERION_CHECK(found);
}

HYPERION_TEST("dispatch", "rule_cannot_override_a_builtin_handler") {
    TempDir dir;
    RegistryRule rule = chdir_rule();
    rule.syscall = 1; // write(2): implemented by the existing switch
    rule.name = "write";
    write_text(dir.file("registry.json"), document_with_rule(rule));

    MinimalSyscallHandler handler({}, nullptr);
    std::string message;
    (void)handler.reload_syscall_registry(options_at(dir.file("registry.json")), message);
    bool found = false;
    for (const std::string& d : handler.dispatcher().diagnostics()) {
        found = found || d.find("already implements this syscall") != std::string::npos;
    }
    HYPERION_CHECK(found);
    HYPERION_CHECK(handler.dispatcher().lookup(1) == nullptr);
}

HYPERION_TEST("dispatch", "rule_cannot_override_the_linux_compat_handler") {
    TempDir dir;
    RegistryRule rule = chdir_rule();
    rule.syscall = 202; // futex: handled by the --linux-compat arm
    rule.name = "futex";
    write_text(dir.file("registry.json"), document_with_rule(rule));

    MinimalSyscallHandler handler({}, nullptr);
    std::string message;
    (void)handler.reload_syscall_registry(options_at(dir.file("registry.json")), message);
    bool found = false;
    for (const std::string& d : handler.dispatcher().diagnostics()) {
        found = found || d.find("already implements this syscall") != std::string::npos;
    }
    HYPERION_CHECK(found);
    HYPERION_CHECK(handler.dispatcher().lookup(202) == nullptr);
}

HYPERION_TEST("dispatch", "declared_transform_must_match_the_adapter") {
    TempDir dir;
    RegistryRule rule = chdir_rule();
    rule.transforms = {"guest_fd"}; // chdir does not translate descriptors
    write_text(dir.file("registry.json"), document_with_rule(rule));

    MinimalSyscallHandler handler({}, nullptr);
    std::string message;
    (void)handler.reload_syscall_registry(options_at(dir.file("registry.json")), message);
    bool found = false;
    for (const std::string& d : handler.dispatcher().diagnostics()) {
        found = found || d.find("does not perform") != std::string::npos;
    }
    HYPERION_CHECK(found);
}

HYPERION_TEST("dispatch", "omitted_transform_is_rejected") {
    TempDir dir;
    RegistryRule rule = chdir_rule();
    rule.transforms.clear();
    write_text(dir.file("registry.json"), document_with_rule(rule));

    MinimalSyscallHandler handler({}, nullptr);
    std::string message;
    (void)handler.reload_syscall_registry(options_at(dir.file("registry.json")), message);
    bool found = false;
    for (const std::string& d : handler.dispatcher().diagnostics()) {
        found = found || d.find("omits transform") != std::string::npos;
    }
    HYPERION_CHECK(found);
}

HYPERION_TEST("dispatch", "unknown_return_convention_is_rejected") {
    TempDir dir;
    RegistryRule rule = chdir_rule();
    rule.return_transform = "positive_errno";
    write_text(dir.file("registry.json"), document_with_rule(rule));

    MinimalSyscallHandler handler({}, nullptr);
    std::string message;
    (void)handler.reload_syscall_registry(options_at(dir.file("registry.json")), message);
    bool found = false;
    for (const std::string& d : handler.dispatcher().diagnostics()) {
        found = found || d.find("unsupported return_transform") != std::string::npos;
    }
    HYPERION_CHECK(found);
}

HYPERION_TEST("dispatch", "host_arch_requirement_is_enforced") {
    TempDir dir;
    RegistryRule rule = chdir_rule();
#if defined(__aarch64__)
    rule.requirements = {"host_arch:x86_64"};
#else
    rule.requirements = {"host_arch:aarch64"};
#endif
    write_text(dir.file("registry.json"), document_with_rule(rule));

    MinimalSyscallHandler handler({}, nullptr);
    std::string message;
    (void)handler.reload_syscall_registry(options_at(dir.file("registry.json")), message);
    bool found = false;
    for (const std::string& d : handler.dispatcher().diagnostics()) {
        found = found || d.find("requires host_arch") != std::string::npos;
    }
    HYPERION_CHECK(found);
}

HYPERION_TEST("dispatch", "alias_target_must_be_implemented") {
    TempDir dir;
    RegistryRule rule;
    rule.id = "user.alias";
    rule.name = "bogus";
    rule.syscall = 400;
    rule.implementation = hyperion::RuleImplementation::kGuestAlias;
    rule.target_syscall = 401; // nothing implements 401
    rule.origin = hyperion::RuleOrigin::kUser;
    write_text(dir.file("registry.json"), document_with_rule(rule));

    MinimalSyscallHandler handler({}, nullptr);
    std::string message;
    (void)handler.reload_syscall_registry(options_at(dir.file("registry.json")), message);
    bool found = false;
    for (const std::string& d : handler.dispatcher().diagnostics()) {
        found = found || d.find("is not implemented by an existing handler") != std::string::npos;
    }
    HYPERION_CHECK(found);
}

HYPERION_TEST("dispatch", "duplicate_rule_ids_and_numbers_are_rejected") {
    TempDir dir;
    RegistryRule first = chdir_rule();
    first.id = "user.a";
    first.syscall = 300;
    first.implementation = hyperion::RuleImplementation::kGuestAlias;
    first.target_syscall = 39; // getpid, implemented by the existing switch
    first.transforms.clear();
    first.handler.clear();
    RegistryRule second = first; // same id
    second.syscall = 301;
    RegistryRule third = first;
    third.id = "user.b"; // same number as first, different id
    hyperion::RegistryDocument document;
    document.rules = {first, second, third};
    write_text(dir.file("registry.json"), hyperion::document_to_text(document));

    MinimalSyscallHandler handler({}, nullptr);
    std::string message;
    (void)handler.reload_syscall_registry(options_at(dir.file("registry.json")), message);
    int duplicate_id = 0;
    int duplicate_number = 0;
    for (const std::string& d : handler.dispatcher().diagnostics()) {
        if (d.find("duplicate rule id") != std::string::npos) {
            ++duplicate_id;
        }
        if (d.find("already claims this syscall number") != std::string::npos) {
            ++duplicate_number;
        }
    }
    HYPERION_CHECK_EQ(duplicate_id, 1);
    HYPERION_CHECK_EQ(duplicate_number, 1);
}

HYPERION_TEST("dispatch", "a_missing_registry_keeps_builtin_rules") {
    RegistryOptions options;
    options.use_environment = false;
    options.explicit_path = "/tmp/hyperion-no-such-directory-please/registry.json";
    options.create_if_missing = false;

    MinimalSyscallHandler handler({}, nullptr);
    std::string message;
    const int status = handler.reload_syscall_registry(options, message);
    HYPERION_CHECK_EQ(status, 1); // not kOk, but HYPERION still runs
    HYPERION_CHECK(handler.dispatcher().rules() != nullptr);
    HYPERION_CHECK(handler.dispatcher().lookup(80) != nullptr); // built-in rule still in service
}

// --- dispatch behaviour through the public handler API -----------------------

HYPERION_TEST("dispatch", "adapter_path_handles_a_syscall_end_to_end") {
    MinimalSyscallHandler handler({}, nullptr);
    hyperion::FlatMemory memory(kBase, kMemSize);

    GuestState state;
    state.reg(Gpr::kRax) = 24; // sched_yield
    const hyperion::SyscallResult result = handler.handle(state, memory);
    HYPERION_CHECK(result.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(state.reg(Gpr::kRax), std::uint64_t{0});
    HYPERION_CHECK_EQ(handler.dispatcher().counters().handled_adapter.load(), std::uint64_t{1});
    HYPERION_CHECK_EQ(handler.dispatcher().counters().handled_existing.load(), std::uint64_t{0});
}

HYPERION_TEST("dispatch", "existing_handler_path_is_still_used_and_counted") {
    MinimalSyscallHandler handler({}, nullptr);
    hyperion::FlatMemory memory(kBase, kMemSize);

    GuestState state;
    state.reg(Gpr::kRax) = 39; // getpid: the existing switch
    const hyperion::SyscallResult result = handler.handle(state, memory);
    HYPERION_CHECK(result.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(handler.dispatcher().counters().handled_existing.load(), std::uint64_t{1});
    HYPERION_CHECK_EQ(handler.dispatcher().counters().handled_adapter.load(), std::uint64_t{0});
}

HYPERION_TEST("dispatch", "unsupported_number_is_enosys_and_recorded") {
    MinimalSyscallHandler handler({}, nullptr);
    hyperion::FlatMemory memory(kBase, kMemSize);

    GuestState state;
    state.reg(Gpr::kRax) = 451; // not implemented anywhere
    const hyperion::SyscallResult result = handler.handle(state, memory);
    HYPERION_CHECK(result.status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(state.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
    HYPERION_CHECK_EQ(handler.dispatcher().counters().unsupported_events.load(), std::uint64_t{1});
    HYPERION_CHECK_EQ(handler.dispatcher().unsupported().unique_count(), std::size_t{1});
    HYPERION_CHECK(handler.dispatcher().unsupported().contains(451));

    // Repeating the same number raises the event count but not the unique count.
    state.reg(Gpr::kRax) = 451;
    (void)handler.handle(state, memory);
    HYPERION_CHECK_EQ(handler.dispatcher().counters().unsupported_events.load(), std::uint64_t{2});
    HYPERION_CHECK_EQ(handler.dispatcher().unsupported().unique_count(), std::size_t{1});
}

HYPERION_TEST("dispatch", "alias_rule_rewrites_arguments_and_restores_them") {
    // getrlimit(97) is shipped as a guest_alias to prlimit64(302), which the
    // existing handler implements. The alias must map the arguments and leave
    // the caller's argument registers untouched.
    MinimalSyscallHandler handler({}, nullptr);
    hyperion::FlatMemory memory(kBase, kMemSize);

    HYPERION_CHECK(handler.dispatcher().lookup(97) != nullptr);
    HYPERION_CHECK(handler.dispatcher().lookup(97)->kind == DispatchKind::kGuestAlias);

    GuestState state;
    state.reg(Gpr::kRax) = 97; // getrlimit
    state.reg(Gpr::kRdi) = 7;  // RLIMIT_NOFILE
    state.reg(Gpr::kRsi) = kBase + 0x100;
    state.reg(Gpr::kRdx) = 0xdead; // must be preserved
    state.reg(Gpr::kR10) = 0xbeef;
    state.reg(Gpr::kR8) = 0xcafe;
    state.reg(Gpr::kR9) = 0xf00d;

    const hyperion::SyscallResult result = handler.handle(state, memory);
    HYPERION_CHECK(result.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(result.number, std::uint64_t{97});
    HYPERION_CHECK_EQ(state.reg(Gpr::kRdi), std::uint64_t{7}); // caller's argument
    HYPERION_CHECK_EQ(state.reg(Gpr::kRdx), std::uint64_t{0xdead});
    HYPERION_CHECK_EQ(state.reg(Gpr::kR10), std::uint64_t{0xbeef});
    HYPERION_CHECK_EQ(state.reg(Gpr::kR8), std::uint64_t{0xcafe});
    HYPERION_CHECK_EQ(state.reg(Gpr::kR9), std::uint64_t{0xf00d});
    HYPERION_CHECK_EQ(handler.dispatcher().counters().handled_alias.load(), std::uint64_t{1});

    // prlimit64 wrote a struct rlimit (two 64-bit fields) into guest memory.
    std::uint8_t raw[16] = {};
    HYPERION_CHECK(memory.read(kBase + 0x100, raw, sizeof(raw)));
    const std::uint64_t soft = hyperion::syscall_detail::get_u64_le(raw);
    HYPERION_CHECK(soft > 0);
}

HYPERION_TEST("dispatch", "user_rule_adds_a_new_syscall_without_touching_others") {
    TempDir dir;
    RegistryRule rule = chdir_rule();
    rule.syscall = 300; // a number nothing else implements
    rule.name = "custom";
    // A builtin_adapter rule must name a handler that implements exactly this
    // number, so a genuinely new syscall is expressed as an alias to one the
    // existing switch already implements.
    rule.implementation = hyperion::RuleImplementation::kGuestAlias;
    rule.target_syscall = 39; // getpid, implemented by the existing switch
    rule.transforms.clear();
    rule.handler.clear();
    rule.arg_source.clear();
    write_text(dir.file("registry.json"), document_with_rule(rule));

    MinimalSyscallHandler handler({}, nullptr);
    std::string message;
    HYPERION_CHECK_EQ(
        handler.reload_syscall_registry(options_at(dir.file("registry.json")), message), 0);

    const hyperion::DispatchEntry* entry = handler.dispatcher().lookup(300);
    HYPERION_CHECK(entry != nullptr);
    HYPERION_CHECK(entry->kind == DispatchKind::kGuestAlias);

    hyperion::FlatMemory memory(kBase, kMemSize);
    GuestState state;
    state.reg(Gpr::kRax) = 300;
    const hyperion::SyscallResult result = handler.handle(state, memory);
    HYPERION_CHECK(result.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(result.number, std::uint64_t{300});
    // The pre-existing behaviour of the unaffected syscalls is unchanged.
    HYPERION_CHECK(handler.dispatcher().lookup(80) != nullptr);
    HYPERION_CHECK(handler.dispatcher().lookup(1) == nullptr);
}

HYPERION_TEST("dispatch", "reload_swaps_the_snapshot_atomically") {
    MinimalSyscallHandler handler({}, nullptr);
    HYPERION_CHECK(handler.dispatcher().lookup(300) == nullptr);

    TempDir dir;
    RegistryRule rule = chdir_rule();
    rule.syscall = 300;
    rule.name = "custom";
    rule.implementation = hyperion::RuleImplementation::kGuestAlias;
    rule.target_syscall = 39;
    rule.transforms.clear();
    rule.handler.clear();
    write_text(dir.file("registry.json"), document_with_rule(rule));

    std::string message;
    HYPERION_CHECK_EQ(
        handler.reload_syscall_registry(options_at(dir.file("registry.json")), message), 0);
    HYPERION_CHECK(handler.dispatcher().lookup(300) != nullptr);

    // A corrupt file must leave the previous snapshot in service.
    write_text(dir.file("registry.json"), "{ not json");
    const int status =
        handler.reload_syscall_registry(options_at(dir.file("registry.json")), message);
    HYPERION_CHECK_EQ(status, 1);
    HYPERION_CHECK(handler.dispatcher().lookup(300) != nullptr);
    HYPERION_CHECK(handler.dispatcher().lookup(80) != nullptr);
}

HYPERION_TEST("dispatch", "summary_names_the_unsupported_syscalls") {
    MinimalSyscallHandler handler({}, nullptr);
    hyperion::FlatMemory memory(kBase, kMemSize);
    GuestState state;
    state.reg(Gpr::kRax) = 451;
    (void)handler.handle(state, memory);
    state.reg(Gpr::kRax) = 202; // futex: implemented by the compat arm only
    (void)handler.handle(state, memory);

    const std::string summary = handler.dispatcher().summary();
    HYPERION_CHECK(summary.find("unsupported=2") != std::string::npos);
    HYPERION_CHECK(summary.find("451") != std::string::npos);
    HYPERION_CHECK(summary.find("futex") != std::string::npos);
}

// --- fork(2) inheritance ----------------------------------------------------
//
// A child process builds its own handler. Before this was fixed it started with
// the built-in rules only, so a rule from the user's registry worked in the
// parent and silently became -ENOSYS after fork(2) - a failure that looks like
// an unrelated bug in the child. The child adopts the parent's immutable
// snapshot instead of re-reading the file (which would be slow and racy against a
// concurrent reload).

namespace {

// A registry document with one user rule mapping syscall 300 onto getpid(2).
RegistryRule custom_rule() {
    RegistryRule rule;
    rule.id = "user.test.fork_inherit";
    rule.name = "custom300";
    rule.syscall = 300;
    rule.implementation = hyperion::RuleImplementation::kGuestAlias;
    rule.target_syscall = 39; // getpid
    rule.origin = hyperion::RuleOrigin::kUser;
    rule.rule_version = 1;
    rule.enabled = true;
    return rule;
}

} // namespace

HYPERION_TEST("syscall_dispatch", "child_adopts_the_parents_compiled_rules") {
    TempDir dir;
    const std::string path = dir.file("registry.json");
    hyperion::RegistryDocument document;
    document.schema_version = hyperion::kRegistrySchemaVersion;
    document.guest_arch = "x86_64";
    document.rules.push_back(custom_rule());
    std::string write_error;
    HYPERION_CHECK(hyperion::write_registry_atomic(path, document, write_error));

    MinimalSyscallHandler parent({}, nullptr);
    RegistryOptions options;
    options.use_environment = false;
    options.explicit_path = path;
    std::string message;
    HYPERION_CHECK_EQ(parent.reload_syscall_registry(options, message), 0);
    HYPERION_CHECK(parent.dispatcher().lookup(300) != nullptr);

    // A child handler, as process.cpp builds it, knows nothing about the file.
    MinimalSyscallHandler child({}, nullptr);
    HYPERION_CHECK(child.dispatcher().lookup(300) == nullptr);

    hyperion::SyscallAccess::inherit_compiled_rules(child, parent);
    const auto* inherited = child.dispatcher().lookup(300);
    HYPERION_CHECK(inherited != nullptr);
    HYPERION_CHECK(inherited->kind == DispatchKind::kGuestAlias);
    // The provenance travels with the snapshot, so a diagnostic in the child
    // names the same file the parent used.
    HYPERION_CHECK(child.dispatcher().registry_path() == path);
    // Counters do not: they belong to the process that ran the syscall.
    HYPERION_CHECK_EQ(child.dispatcher().counters().unsupported_events.load(), std::uint64_t{0});
}

HYPERION_TEST("syscall_dispatch", "child_inheritance_survives_a_parent_reload") {
    TempDir dir;
    const std::string path = dir.file("registry.json");
    hyperion::RegistryDocument document;
    document.schema_version = hyperion::kRegistrySchemaVersion;
    document.guest_arch = "x86_64";
    document.rules.push_back(custom_rule());
    std::string write_error;
    HYPERION_CHECK(hyperion::write_registry_atomic(path, document, write_error));

    MinimalSyscallHandler parent({}, nullptr);
    RegistryOptions options;
    options.use_environment = false;
    options.explicit_path = path;
    std::string message;
    HYPERION_CHECK_EQ(parent.reload_syscall_registry(options, message), 0);

    MinimalSyscallHandler child({}, nullptr);
    hyperion::SyscallAccess::inherit_compiled_rules(child, parent);

    // The child holds the snapshot it adopted: a later reload in the parent
    // publishes a new immutable snapshot rather than mutating the old one, so a
    // syscall already in flight in the child cannot observe a half-swapped table.
    hyperion::RegistryDocument empty;
    empty.schema_version = hyperion::kRegistrySchemaVersion;
    empty.guest_arch = "x86_64";
    HYPERION_CHECK(hyperion::write_registry_atomic(path, empty, write_error));
    HYPERION_CHECK_EQ(parent.reload_syscall_registry(options, message), 0);
    HYPERION_CHECK(parent.dispatcher().lookup(300) == nullptr);
    HYPERION_CHECK(child.dispatcher().lookup(300) != nullptr);
}
