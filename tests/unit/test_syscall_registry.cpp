// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Tests for the persistent syscall registry: path resolution, creation,
// migration, merging, validation and crash-safe replacement.
//
// Every test works inside a private temporary directory so nothing touches the
// developer's real $HOME/.local/share/hyperion.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "harness/test_harness.hpp"
#include "harness/test_tempdir.hpp"
#include "hyperion/json.hpp"
#include "hyperion/syscall_registry.hpp"

using hyperion::RegistryDocument;
using hyperion::RegistryOptions;
using hyperion::RegistryPathInfo;
using hyperion::RegistryReport;
using hyperion::RegistryRule;
using hyperion::test::TempDir;

namespace {

// Set an environment variable for the duration of a scope.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        const char* previous = std::getenv(name);
        if (previous != nullptr) {
            had_previous_ = true;
            previous_ = previous;
        }
        if (value == nullptr) {
            ::unsetenv(name);
        } else {
            ::setenv(name, value, 1);
        }
    }
    ~ScopedEnv() {
        if (had_previous_) {
            ::setenv(name_.c_str(), previous_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string name_;
    std::string previous_;
    bool had_previous_ = false;
};

RegistryRule user_rule(std::uint64_t syscall, const std::string& name) {
    RegistryRule rule;
    rule.id = "user." + name;
    rule.name = name;
    rule.syscall = syscall;
    rule.implementation = hyperion::RuleImplementation::kBuiltinAdapter;
    rule.handler = "compat.chdir";
    rule.transforms = {"path.sysroot"};
    rule.origin = hyperion::RuleOrigin::kUser;
    rule.enabled = true;
    return rule;
}

} // namespace

// --- path resolution ---------------------------------------------------------

HYPERION_TEST("registry", "explicit_path_wins") {
    RegistryOptions options;
    options.explicit_path = "/tmp/explicit/registry.json";
    const RegistryPathInfo info = hyperion::resolve_registry_path(options);
    HYPERION_CHECK_EQ(info.path, std::string("/tmp/explicit/registry.json"));
    HYPERION_CHECK_EQ(info.source, std::string("override"));
}

HYPERION_TEST("registry", "environment_override_is_used") {
    ScopedEnv override_env("HYPERION_SYSCALL_REGISTRY", "/tmp/from-env/registry.json");
    RegistryOptions options;
    const RegistryPathInfo info = hyperion::resolve_registry_path(options);
    HYPERION_CHECK_EQ(info.path, std::string("/tmp/from-env/registry.json"));
    HYPERION_CHECK_EQ(info.source, std::string("override"));
    HYPERION_CHECK(info.reason.find("HYPERION_SYSCALL_REGISTRY") != std::string::npos);
}

HYPERION_TEST("registry", "xdg_data_home_is_preferred_over_home") {
    ScopedEnv override_env("HYPERION_SYSCALL_REGISTRY", nullptr);
    ScopedEnv xdg("XDG_DATA_HOME", "/tmp/xdg-data");
    ScopedEnv home("HOME", "/tmp/home-dir");
    const RegistryPathInfo info = hyperion::resolve_registry_path(RegistryOptions{});
    HYPERION_CHECK_EQ(info.path, std::string("/tmp/xdg-data/hyperion/syscalls/registry.json"));
    HYPERION_CHECK_EQ(info.source, std::string("xdg"));
}

HYPERION_TEST("registry", "home_fallback_is_the_documented_xdg_default") {
    ScopedEnv override_env("HYPERION_SYSCALL_REGISTRY", nullptr);
    ScopedEnv xdg("XDG_DATA_HOME", nullptr);
    ScopedEnv home("HOME", "/tmp/home-dir");
    const RegistryPathInfo info = hyperion::resolve_registry_path(RegistryOptions{});
    HYPERION_CHECK_EQ(info.path,
                      std::string("/tmp/home-dir/.local/share/hyperion/syscalls/registry.json"));
    HYPERION_CHECK_EQ(info.source, std::string("home"));
}

HYPERION_TEST("registry", "no_location_degrades_cleanly") {
    ScopedEnv override_env("HYPERION_SYSCALL_REGISTRY", nullptr);
    ScopedEnv xdg("XDG_DATA_HOME", nullptr);
    ScopedEnv home("HOME", nullptr);
    const RegistryPathInfo info = hyperion::resolve_registry_path(RegistryOptions{});
    HYPERION_CHECK(info.path.empty());
    HYPERION_CHECK_EQ(info.source, std::string("none"));

    RegistryDocument document;
    const RegistryReport report = hyperion::load_registry(RegistryOptions{}, document);
    HYPERION_CHECK(report.status == RegistryReport::Status::kNoLocation);
    // Startup must still succeed with the built-in rules.
    HYPERION_CHECK(report.ok());
    HYPERION_CHECK(!document.rules.empty());
}

// --- creation and persistence ------------------------------------------------

HYPERION_TEST("registry", "creates_defaults_with_owner_only_permissions") {
    TempDir dir;
    RegistryOptions options;
    options.explicit_path = dir.file("registry.json");
    RegistryDocument document;
    const RegistryReport report = hyperion::load_registry(options, document);

    HYPERION_CHECK(report.status == RegistryReport::Status::kOk);
    HYPERION_CHECK(report.created);
    HYPERION_CHECK(!document.rules.empty());

    struct stat info{};
    HYPERION_CHECK_EQ(::stat(options.explicit_path.c_str(), &info), 0);
    HYPERION_CHECK_EQ(info.st_mode & 0777, static_cast<mode_t>(0600));
}

HYPERION_TEST("registry", "written_file_round_trips_through_the_parser") {
    TempDir dir;
    RegistryOptions options;
    options.explicit_path = dir.file("registry.json");
    RegistryDocument document;
    (void)hyperion::load_registry(options, document);

    RegistryDocument reread;
    const RegistryReport second = hyperion::load_registry(options, reread);
    HYPERION_CHECK(!second.created);
    HYPERION_CHECK_EQ(reread.rules.size(), document.rules.size());
    HYPERION_CHECK_EQ(reread.schema_version, hyperion::kRegistrySchemaVersion);
}

HYPERION_TEST("registry", "atomic_write_keeps_the_previous_copy_as_bak") {
    TempDir dir;
    const std::string path = dir.file("registry.json");

    RegistryDocument first;
    first.created_unix = 1;
    first.updated_unix = 1;
    first.rules.push_back(user_rule(80, "chdir"));
    std::string error;
    HYPERION_CHECK(hyperion::write_registry_atomic(path, first, error));

    RegistryDocument second;
    second.created_unix = 1;
    second.updated_unix = 2;
    second.rules.push_back(user_rule(81, "fchdir"));
    HYPERION_CHECK(hyperion::write_registry_atomic(path, second, error));

    std::string backup;
    HYPERION_CHECK(hyperion::read_text_file(path + ".bak", backup, error));
    HYPERION_CHECK(backup.find("user.chdir") != std::string::npos);
    HYPERION_CHECK(backup.find("user.fchdir") == std::string::npos);

    std::string current;
    HYPERION_CHECK(hyperion::read_text_file(path, current, error));
    HYPERION_CHECK(current.find("user.fchdir") != std::string::npos);
}

HYPERION_TEST("registry", "interrupted_write_leaves_no_partial_file") {
    // A temporary file left behind by a crash must not be mistaken for the
    // registry, and the registry itself must be untouched.
    TempDir dir;
    const std::string path = dir.file("registry.json");
    RegistryDocument document;
    document.rules.push_back(user_rule(80, "chdir"));
    std::string error;
    HYPERION_CHECK(hyperion::write_registry_atomic(path, document, error));

    std::string before;
    HYPERION_CHECK(hyperion::read_text_file(path, before, error));
    const std::string stale_temp = path + ".tmp.999999";
    HYPERION_CHECK_EQ(::system(("printf '{\"trunc' > '" + stale_temp + "'").c_str()), 0);

    RegistryOptions options;
    options.explicit_path = path;
    RegistryDocument loaded;
    const RegistryReport explicit_report = hyperion::load_registry(options, loaded);
    HYPERION_CHECK(explicit_report.status == RegistryReport::Status::kOk);
    std::string after;
    HYPERION_CHECK(hyperion::read_text_file(path, after, error));
    HYPERION_CHECK_EQ(before, after);
}

// --- schema validation, migration, merging -----------------------------------

HYPERION_TEST("registry", "corrupt_json_is_reported_and_the_file_is_kept") {
    TempDir dir;
    const std::string path = dir.file("registry.json");
    const std::string corrupt = "{\"schema_version\": 2, \"rules\": [";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    HYPERION_CHECK(f != nullptr);
    HYPERION_CHECK_EQ(std::fwrite(corrupt.data(), 1, corrupt.size(), f), corrupt.size());
    std::fclose(f);

    RegistryOptions options;
    options.explicit_path = path;
    RegistryDocument document;
    const RegistryReport report = hyperion::load_registry(options, document);

    HYPERION_CHECK(report.status == RegistryReport::Status::kInvalid);
    HYPERION_CHECK(!report.errors.empty());
    // The built-in rules are still in service, so HYPERION still runs.
    HYPERION_CHECK(!document.rules.empty());
    // The corrupt file was not overwritten.
    std::string contents;
    std::string error;
    HYPERION_CHECK(hyperion::read_text_file(path, contents, error));
    HYPERION_CHECK_EQ(contents, corrupt);
}

HYPERION_TEST("registry", "newer_schema_is_refused_not_downgraded") {
    const std::string text = "{\"schema_version\": 99, \"guest_arch\": \"x86_64\", \"rules\": []}";
    hyperion::json::Value root;
    hyperion::json::ParseError parse_error;
    HYPERION_CHECK(hyperion::json::parse(text, root, parse_error));
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    const RegistryDocument document = hyperion::document_from_json(root, errors, warnings);
    HYPERION_CHECK(!errors.empty());
    HYPERION_CHECK(errors[0].find("newer than this build supports") != std::string::npos);

    RegistryDocument mutable_document = document;
    mutable_document.schema_version = 99;
    std::string error;
    HYPERION_CHECK(!hyperion::migrate_document(mutable_document, warnings, error));
    HYPERION_CHECK(error.find("newer than this build") != std::string::npos);
}

HYPERION_TEST("registry", "schema_version_1_is_migrated_in_memory") {
    const std::string text = R"({
        "schema_version": 1,
        "guest_arch": "x86_64",
        "rules": [{"id": "user.old", "name": "chdir", "syscall": 80,
                   "implementation": "builtin_adapter", "handler": "compat.chdir",
                   "transforms": ["path.sysroot"]}]
    })";
    hyperion::json::Value root;
    hyperion::json::ParseError parse_error;
    HYPERION_CHECK(hyperion::json::parse(text, root, parse_error));
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    RegistryDocument document = hyperion::document_from_json(root, errors, warnings);
    HYPERION_CHECK(errors.empty());

    std::string error;
    HYPERION_CHECK(hyperion::migrate_document(document, warnings, error));
    HYPERION_CHECK_EQ(document.schema_version, hyperion::kRegistrySchemaVersion);
    HYPERION_CHECK(!warnings.empty());
    HYPERION_CHECK_EQ(document.rules[0].origin_detail,
                      std::string("migrated from schema_version 1"));
}

HYPERION_TEST("registry", "user_rules_survive_a_simulated_update") {
    RegistryDocument installed = hyperion::default_registry_document();
    RegistryRule mine = user_rule(80, "chdir");
    mine.notes = "hand written by the user";
    installed.rules.push_back(mine);

    // Simulate a HYPERION update: a shipped default gains a rule_version bump.
    RegistryDocument shipped = hyperion::default_registry_document();
    HYPERION_CHECK(!shipped.rules.empty());
    shipped.rules[0].rule_version = 2;
    shipped.rules[0].notes = "updated by the package";
    const std::string upgraded_id = shipped.rules[0].id;
    const std::string user_id = mine.id;

    std::vector<std::string> warnings;
    hyperion::merge_default_rules(installed, shipped, warnings);

    bool user_rule_found = false;
    bool package_rule_upgraded = false;
    for (const RegistryRule& rule : installed.rules) {
        if (rule.id == user_id) {
            user_rule_found = true;
            HYPERION_CHECK_EQ(rule.notes, std::string("hand written by the user"));
            HYPERION_CHECK(rule.origin == hyperion::RuleOrigin::kUser);
        }
        if (rule.id == upgraded_id) {
            package_rule_upgraded = rule.rule_version == 2;
        }
    }
    HYPERION_CHECK(user_rule_found);
    HYPERION_CHECK(package_rule_upgraded);
}

HYPERION_TEST("registry", "a_user_rule_shadows_a_default_with_the_same_id") {
    RegistryDocument document = hyperion::default_registry_document();
    HYPERION_CHECK(!document.rules.empty());
    document.rules[0].origin = hyperion::RuleOrigin::kUser;
    document.rules[0].notes = "user override";
    const std::string id = document.rules[0].id;

    RegistryDocument shipped = hyperion::default_registry_document();
    shipped.rules[0].notes = "package default";
    shipped.rules[0].rule_version = 5;

    std::vector<std::string> warnings;
    hyperion::merge_default_rules(document, shipped, warnings);

    int matches = 0;
    for (const RegistryRule& rule : document.rules) {
        if (rule.id == id) {
            ++matches;
            HYPERION_CHECK_EQ(rule.notes, std::string("user override"));
            HYPERION_CHECK_EQ(rule.rule_version, 1);
        }
    }
    HYPERION_CHECK_EQ(matches, 1);
    bool warned = false;
    for (const std::string& w : warnings) {
        warned = warned || w.find("user rule with that id is present") != std::string::npos;
    }
    HYPERION_CHECK(warned);
}

HYPERION_TEST("registry", "missing_required_fields_are_errors") {
    const std::string text = R"({"schema_version": 2, "rules": [{"name": "x"}]})";
    hyperion::json::Value root;
    hyperion::json::ParseError parse_error;
    HYPERION_CHECK(hyperion::json::parse(text, root, parse_error));
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    (void)hyperion::document_from_json(root, errors, warnings);
    HYPERION_CHECK(!errors.empty());
}

HYPERION_TEST("registry", "wrong_guest_arch_is_refused") {
    const std::string text = R"({"schema_version": 2, "guest_arch": "aarch64", "rules": []})";
    hyperion::json::Value root;
    hyperion::json::ParseError parse_error;
    HYPERION_CHECK(hyperion::json::parse(text, root, parse_error));

    TempDir dir;
    const std::string path = dir.file("registry.json");
    std::FILE* f = std::fopen(path.c_str(), "wb");
    HYPERION_CHECK(f != nullptr);
    HYPERION_CHECK_EQ(std::fwrite(text.data(), 1, text.size(), f), text.size());
    std::fclose(f);

    RegistryOptions options;
    options.explicit_path = path;
    RegistryDocument document;
    const RegistryReport report = hyperion::load_registry(options, document);
    HYPERION_CHECK(report.status == RegistryReport::Status::kInvalid);
    HYPERION_CHECK(report.message.find("guest architecture") != std::string::npos);
}

HYPERION_TEST("registry", "document_round_trips_through_json") {
    RegistryDocument document;
    document.rules.push_back(user_rule(80, "chdir"));
    const std::string text = hyperion::document_to_text(document);
    hyperion::json::Value root;
    hyperion::json::ParseError parse_error;
    HYPERION_CHECK(hyperion::json::parse(text, root, parse_error));
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    const RegistryDocument parsed = hyperion::document_from_json(root, errors, warnings);
    HYPERION_CHECK(errors.empty());
    HYPERION_CHECK_EQ(parsed.rules.size(), std::size_t{1});
    HYPERION_CHECK_EQ(parsed.rules[0].id, std::string("user.chdir"));
    HYPERION_CHECK_EQ(parsed.rules[0].syscall, std::uint64_t{80});
    HYPERION_CHECK_EQ(parsed.rules[0].transforms.size(), std::size_t{1});
    HYPERION_CHECK(parsed.rules[0].origin == hyperion::RuleOrigin::kUser);
}
