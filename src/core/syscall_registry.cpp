// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Persistent syscall registry: location resolution, JSON (de)serialisation,
// schema migration, default merging and crash-safe writes.
//
// Nothing in this file runs on the syscall hot path. It is called once when a
// MinimalSyscallHandler is constructed, and again only on an explicit reload or
// from `hyperion syscall-registry`.
#include "hyperion/syscall_registry.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace hyperion {

namespace {

std::int64_t now_unix() {
    return static_cast<std::int64_t>(::time(nullptr));
}

std::string get_env(const char* name) {
    const char* v = std::getenv(name);
    return v == nullptr ? std::string() : std::string(v);
}

bool is_absolute(const std::string& path) {
    return !path.empty() && path.front() == '/';
}

// Join two path fragments without doubling or dropping the separator.
std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    if (a.back() == '/') {
        return a + b;
    }
    return a + "/" + b;
}

std::string parent_directory(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return std::string(".");
    }
    if (slash == 0) {
        return std::string("/");
    }
    return path.substr(0, slash);
}

// mkdir -p with 0700 for the directories this program creates. Existing
// directories are left with their current permissions.
bool make_directories(const std::string& path, std::string& error) {
    if (path.empty() || path == "/" || path == ".") {
        return true;
    }
    std::string partial;
    std::size_t pos = 0;
    if (path.front() == '/') {
        partial = "/";
        pos = 1;
    }
    while (pos <= path.size()) {
        const std::size_t next = path.find('/', pos);
        const std::string component =
            (next == std::string::npos) ? path.substr(pos) : path.substr(pos, next - pos);
        if (!component.empty()) {
            partial = (partial.empty() || partial == "/") ? partial + component
                                                          : partial + "/" + component;
            if (::mkdir(partial.c_str(), 0700) != 0 && errno != EEXIST) {
                error = "cannot create directory '" + partial + "': " + std::strerror(errno);
                return false;
            }
        }
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
    return true;
}

bool write_all(int fd, const std::string& data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool write_file_atomic_in_dir(const std::string& path, const std::string& contents,
                              std::string& error) {
    const std::string dir = parent_directory(path);
    std::string dir_error;
    if (!make_directories(dir, dir_error)) {
        error = dir_error;
        return false;
    }
    const std::string temp = path + ".tmp." + std::to_string(static_cast<long>(::getpid()));
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = "cannot create '" + temp + "': " + std::strerror(errno);
        return false;
    }
    if (!write_all(fd, contents)) {
        error = "cannot write '" + temp + "': " + std::strerror(errno);
        ::close(fd);
        (void)::unlink(temp.c_str());
        return false;
    }
    // Durability before the rename: a crash between rename and fsync may leave
    // an empty file at the destination on some filesystems.
    if (::fsync(fd) != 0) {
        error = "cannot flush '" + temp + "': " + std::strerror(errno);
        ::close(fd);
        (void)::unlink(temp.c_str());
        return false;
    }
    ::close(fd);
    if (::rename(temp.c_str(), path.c_str()) != 0) {
        error = "cannot replace '" + path + "': " + std::strerror(errno);
        (void)::unlink(temp.c_str());
        return false;
    }
    // Persist the directory entry itself, so the rename is not lost on power
    // failure. Best effort: some filesystems refuse to open a directory.
    const int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
        (void)::fsync(dir_fd);
        ::close(dir_fd);
    }
    return true;
}

// --- JSON conversion --------------------------------------------------------

bool read_string_field(const json::Value& object, std::string_view key, std::string& out,
                       bool required, std::vector<std::string>& errors,
                       const std::string& context) {
    const json::Value* v = object.find(key);
    if (v == nullptr) {
        if (required) {
            errors.push_back(context + ": missing required field '" + std::string(key) + "'");
            return false;
        }
        return true;
    }
    if (!v->is_string()) {
        errors.push_back(context + ": field '" + std::string(key) + "' must be a string");
        return false;
    }
    out = v->string;
    return true;
}

bool read_integer_field(const json::Value& object, std::string_view key, std::int64_t& out,
                        bool required, std::vector<std::string>& errors,
                        const std::string& context) {
    const json::Value* v = object.find(key);
    if (v == nullptr) {
        if (required) {
            errors.push_back(context + ": missing required field '" + std::string(key) + "'");
            return false;
        }
        return true;
    }
    if (!v->is_number()) {
        errors.push_back(context + ": field '" + std::string(key) + "' must be a number");
        return false;
    }
    if (!v->is_integer) {
        errors.push_back(context + ": field '" + std::string(key) +
                         "' must be an integer, not a floating-point value");
        return false;
    }
    out = v->integer;
    return true;
}

bool read_bool_field(const json::Value& object, std::string_view key, bool& out,
                     std::vector<std::string>& errors, const std::string& context) {
    const json::Value* v = object.find(key);
    if (v == nullptr) {
        return true;
    }
    if (!v->is_bool()) {
        errors.push_back(context + ": field '" + std::string(key) + "' must be a boolean");
        return false;
    }
    out = v->boolean;
    return true;
}

bool read_string_array(const json::Value& object, std::string_view key,
                       std::vector<std::string>& out, std::vector<std::string>& errors,
                       const std::string& context) {
    const json::Value* v = object.find(key);
    if (v == nullptr) {
        return true;
    }
    if (!v->is_array()) {
        errors.push_back(context + ": field '" + std::string(key) + "' must be an array");
        return false;
    }
    for (const json::Value& element : v->array) {
        if (!element.is_string()) {
            errors.push_back(context + ": field '" + std::string(key) +
                             "' must contain only strings");
            return false;
        }
        out.push_back(element.string);
    }
    return true;
}

json::Value strings_to_json(const std::vector<std::string>& values) {
    json::Value array = json::Value::make_array();
    for (const std::string& v : values) {
        array.push(json::Value::make_string(v));
    }
    return array;
}

void report_unknown_fields(const json::Value& object, const std::vector<std::string_view>& known,
                           std::vector<std::string>& warnings, const std::string& context) {
    for (const auto& member : object.object) {
        const bool is_known =
            std::find(known.begin(), known.end(), std::string_view(member.key)) != known.end();
        if (!is_known) {
            warnings.push_back(context + ": ignoring unknown field '" + member.key +
                               "' (kept for forward compatibility, not interpreted)");
        }
    }
}

RegistryRule rule_from_json(const json::Value& value, std::vector<std::string>& errors,
                            std::vector<std::string>& warnings, std::size_t index) {
    RegistryRule rule;
    const std::string context = "rules[" + std::to_string(index) + "]";
    if (!value.is_object()) {
        errors.push_back(context + ": rule must be a JSON object");
        return rule;
    }

    std::int64_t syscall_number = -1;
    (void)read_string_field(value, "id", rule.id, true, errors, context);
    (void)read_string_field(value, "name", rule.name, true, errors, context);
    (void)read_integer_field(value, "syscall", syscall_number, true, errors, context);
    if (syscall_number >= 0) {
        rule.syscall = static_cast<std::uint64_t>(syscall_number);
    }

    std::string implementation_text;
    if (read_string_field(value, "implementation", implementation_text, true, errors, context)) {
        if (!parse_rule_implementation(implementation_text, rule.implementation)) {
            errors.push_back(context + ": unknown implementation '" + implementation_text +
                             "' (expected builtin_adapter, guest_alias or disable)");
        }
    }
    (void)read_string_field(value, "handler", rule.handler, false, errors, context);
    std::int64_t target = -1;
    (void)read_integer_field(value, "target_syscall", target, false, errors, context);
    if (target >= 0) {
        rule.target_syscall = static_cast<std::uint64_t>(target);
    } else if (target < -1) {
        errors.push_back(context + ": 'target_syscall' must not be negative");
    }

    const json::Value* arg_source = value.find("arg_source");
    if (arg_source != nullptr) {
        if (!arg_source->is_array()) {
            errors.push_back(context + ": 'arg_source' must be an array");
        } else {
            for (const json::Value& element : arg_source->array) {
                AliasArg a;
                if (!element.is_object()) {
                    errors.push_back(context + ": each 'arg_source' entry must be an object");
                    continue;
                }
                const json::Value* c = element.find("const");
                const json::Value* a_ref = element.find("arg");
                if ((c == nullptr) == (a_ref == nullptr)) {
                    errors.push_back(context +
                                     ": each 'arg_source' entry needs exactly one of 'const' or "
                                     "'arg'");
                    continue;
                }
                if (c != nullptr) {
                    if (!c->is_number() || !c->is_integer) {
                        errors.push_back(context + ": 'const' must be an integer");
                        continue;
                    }
                    a.kind = AliasArg::Kind::kConst;
                    a.value = c->integer;
                } else {
                    if (!a_ref->is_number() || !a_ref->is_integer || a_ref->integer < 0 ||
                        a_ref->integer > 5) {
                        errors.push_back(context + ": 'arg' must be an integer in 0..5");
                        continue;
                    }
                    a.kind = AliasArg::Kind::kArg;
                    a.value = a_ref->integer;
                }
                rule.arg_source.push_back(a);
            }
        }
    }

    (void)read_string_array(value, "transforms", rule.transforms, errors, context);
    (void)read_string_array(value, "requires", rule.requirements, errors, context);
    (void)read_string_field(value, "return_transform", rule.return_transform, false, errors,
                            context);
    (void)read_string_field(value, "notes", rule.notes, false, errors, context);
    (void)read_string_field(value, "origin_detail", rule.origin_detail, false, errors, context);
    (void)read_bool_field(value, "enabled", rule.enabled, errors, context);

    std::int64_t rule_version = rule.rule_version;
    (void)read_integer_field(value, "rule_version", rule_version, false, errors, context);
    if (rule_version < 1 || rule_version > 1'000'000) {
        errors.push_back(context + ": 'rule_version' must be a positive integer");
    } else {
        rule.rule_version = static_cast<int>(rule_version);
    }

    std::string origin_text;
    if (read_string_field(value, "origin", origin_text, false, errors, context)) {
        if (!origin_text.empty() && !parse_rule_origin(origin_text, rule.origin)) {
            errors.push_back(context + ": unknown origin '" + origin_text +
                             "' (expected package or user)");
        }
    }

    report_unknown_fields(value,
                          {"id", "name", "syscall", "implementation", "handler", "target_syscall",
                           "arg_source", "transforms", "requires", "return_transform", "notes",
                           "origin_detail", "enabled", "rule_version", "origin"},
                          warnings, context);
    return rule;
}

json::Value rule_to_json(const RegistryRule& rule) {
    json::Value v = json::Value::make_object();
    v.set("id", json::Value::make_string(rule.id));
    v.set("name", json::Value::make_string(rule.name));
    v.set("syscall", json::Value::make_int(static_cast<std::int64_t>(rule.syscall)));
    v.set("implementation", json::Value::make_string(to_string(rule.implementation)));
    if (!rule.handler.empty()) {
        v.set("handler", json::Value::make_string(rule.handler));
    }
    if (rule.implementation == RuleImplementation::kGuestAlias) {
        v.set("target_syscall",
              json::Value::make_int(static_cast<std::int64_t>(rule.target_syscall)));
        json::Value args = json::Value::make_array();
        for (const AliasArg& a : rule.arg_source) {
            json::Value entry = json::Value::make_object();
            if (a.kind == AliasArg::Kind::kConst) {
                entry.set("const", json::Value::make_int(a.value));
            } else {
                entry.set("arg", json::Value::make_int(a.value));
            }
            args.push(entry);
        }
        if (!args.array.empty()) {
            v.set("arg_source", args);
        }
    }
    if (!rule.transforms.empty()) {
        v.set("transforms", strings_to_json(rule.transforms));
    }
    v.set("return_transform", json::Value::make_string(rule.return_transform));
    if (!rule.requirements.empty()) {
        v.set("requires", strings_to_json(rule.requirements));
    }
    v.set("rule_version", json::Value::make_int(rule.rule_version));
    v.set("origin", json::Value::make_string(to_string(rule.origin)));
    if (!rule.origin_detail.empty()) {
        v.set("origin_detail", json::Value::make_string(rule.origin_detail));
    }
    v.set("enabled", json::Value::make_bool(rule.enabled));
    if (!rule.notes.empty()) {
        v.set("notes", json::Value::make_string(rule.notes));
    }
    return v;
}

} // namespace

const char* to_string(RuleImplementation impl) {
    switch (impl) {
    case RuleImplementation::kBuiltinAdapter:
        return "builtin_adapter";
    case RuleImplementation::kGuestAlias:
        return "guest_alias";
    case RuleImplementation::kDisable:
        return "disable";
    }
    return "unknown";
}

bool parse_rule_implementation(const std::string& text, RuleImplementation& out) {
    if (text == "builtin_adapter") {
        out = RuleImplementation::kBuiltinAdapter;
        return true;
    }
    if (text == "guest_alias") {
        out = RuleImplementation::kGuestAlias;
        return true;
    }
    if (text == "disable") {
        out = RuleImplementation::kDisable;
        return true;
    }
    return false;
}

const char* to_string(RuleOrigin origin) {
    switch (origin) {
    case RuleOrigin::kPackage:
        return "package";
    case RuleOrigin::kUser:
        return "user";
    }
    return "user";
}

bool parse_rule_origin(const std::string& text, RuleOrigin& out) {
    if (text == "package") {
        out = RuleOrigin::kPackage;
        return true;
    }
    if (text == "user") {
        out = RuleOrigin::kUser;
        return true;
    }
    return false;
}

RegistryPathInfo resolve_registry_path(const RegistryOptions& options) {
    RegistryPathInfo info;
    if (!options.explicit_path.empty()) {
        info.path = options.explicit_path;
        info.source = "override";
        info.reason = "path given explicitly on the command line";
        return info;
    }
    if (options.use_environment) {
        const std::string override_path = get_env("HYPERION_SYSCALL_REGISTRY");
        if (!override_path.empty()) {
            if (is_absolute(override_path)) {
                info.path = override_path;
            } else {
                // A relative override is resolved against the current working
                // directory rather than silently ignored, but it is made
                // absolute so a later chdir cannot retarget it.
                char cwd[4096];
                if (::getcwd(cwd, sizeof(cwd)) != nullptr) {
                    info.path = join_path(cwd, override_path);
                } else {
                    info.path = override_path;
                }
            }
            info.source = "override";
            info.reason = "$HYPERION_SYSCALL_REGISTRY";
            return info;
        }
    }
    const std::string xdg = get_env("XDG_DATA_HOME");
    if (!xdg.empty() && is_absolute(xdg)) {
        info.path = join_path(join_path(xdg, "hyperion/syscalls"), "registry.json");
        info.source = "xdg";
        info.reason = "$XDG_DATA_HOME";
        return info;
    }
    const std::string home = get_env("HOME");
    if (!home.empty() && is_absolute(home)) {
        info.path = join_path(join_path(home, ".local/share/hyperion/syscalls"), "registry.json");
        info.source = "home";
        info.reason = "$HOME/.local/share (XDG default; also the Termux app-private location)";
        return info;
    }
    info.path.clear();
    info.source = "none";
    info.reason = "no $HYPERION_SYSCALL_REGISTRY, $XDG_DATA_HOME or absolute $HOME; "
                  "running with built-in rules only";
    return info;
}

bool read_text_file(const std::string& path, std::string& out, std::string& error,
                    std::size_t max_bytes) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = std::strerror(errno);
        return false;
    }
    out.clear();
    char buffer[8192];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
        if (out.size() + n > max_bytes) {
            std::fclose(f);
            error = "file exceeds the maximum accepted size";
            return false;
        }
        out.append(buffer, n);
    }
    const bool read_error = std::ferror(f) != 0;
    std::fclose(f);
    if (read_error) {
        error = "read error";
        return false;
    }
    return true;
}

RegistryDocument document_from_json(const json::Value& root, std::vector<std::string>& errors,
                                    std::vector<std::string>& warnings) {
    RegistryDocument document;
    if (!root.is_object()) {
        errors.push_back("document: top-level value must be a JSON object");
        return document;
    }
    std::int64_t schema = 0;
    if (!read_integer_field(root, "schema_version", schema, true, errors, "document")) {
        return document;
    }
    if (schema < kRegistryMinSchemaVersion) {
        errors.push_back("document: unsupported schema_version " + std::to_string(schema) +
                         " (minimum supported is " + std::to_string(kRegistryMinSchemaVersion) +
                         ")");
        return document;
    }
    if (schema > kRegistrySchemaVersion) {
        // Never interpret a document written for a newer HYPERION: the fields
        // it relies on may not exist yet, and silently ignoring them could
        // change which syscalls are claimed.
        errors.push_back("document: schema_version " + std::to_string(schema) +
                         " is newer than this build supports (" +
                         std::to_string(kRegistrySchemaVersion) + "); the file was left untouched");
        return document;
    }
    document.schema_version = static_cast<int>(schema);

    (void)read_string_field(root, "guest_arch", document.guest_arch, false, errors, "document");
    (void)read_string_field(root, "host_arch", document.host_arch, false, errors, "document");
    (void)read_string_field(root, "generated_by", document.generated_by, false, errors, "document");
    (void)read_string_field(root, "comment", document.comment, false, errors, "document");
    (void)read_integer_field(root, "created_unix", document.created_unix, false, errors,
                             "document");
    (void)read_integer_field(root, "updated_unix", document.updated_unix, false, errors,
                             "document");

    const json::Value* rules = root.find("rules");
    if (rules == nullptr) {
        errors.push_back("document: missing required field 'rules'");
        return document;
    }
    if (!rules->is_array()) {
        errors.push_back("document: field 'rules' must be an array");
        return document;
    }
    for (std::size_t i = 0; i < rules->array.size(); ++i) {
        document.rules.push_back(
            rule_from_json(rules->array[i], errors, warnings, static_cast<std::size_t>(i)));
    }
    report_unknown_fields(root,
                          {"schema_version", "guest_arch", "host_arch", "generated_by", "comment",
                           "created_unix", "updated_unix", "rules", "metadata"},
                          warnings, "document");
    return document;
}

json::Value document_to_json(const RegistryDocument& document) {
    json::Value root = json::Value::make_object();
    root.set("schema_version", json::Value::make_int(document.schema_version));
    root.set("guest_arch", json::Value::make_string(document.guest_arch));
    if (!document.host_arch.empty()) {
        root.set("host_arch", json::Value::make_string(document.host_arch));
    }
    if (!document.generated_by.empty()) {
        root.set("generated_by", json::Value::make_string(document.generated_by));
    }
    if (!document.comment.empty()) {
        root.set("comment", json::Value::make_string(document.comment));
    }
    root.set("created_unix", json::Value::make_int(document.created_unix));
    root.set("updated_unix", json::Value::make_int(document.updated_unix));
    json::Value rules = json::Value::make_array();
    for (const RegistryRule& rule : document.rules) {
        rules.push(rule_to_json(rule));
    }
    root.set("rules", rules);
    return root;
}

bool migrate_document(RegistryDocument& document, std::vector<std::string>& warnings,
                      std::string& error) {
    if (document.schema_version == kRegistrySchemaVersion) {
        return true;
    }
    if (document.schema_version > kRegistrySchemaVersion) {
        error = "registry schema_version " + std::to_string(document.schema_version) +
                " is newer than this build supports (" + std::to_string(kRegistrySchemaVersion) +
                "); the file was left untouched";
        return false;
    }
    // Version 1 -> 2: per-rule `origin`, `rule_version` and `arg_source` became
    // explicit. v1 rules were implicitly package rules with version 1 and no
    // argument remapping, which is exactly what the parser's defaults produce,
    // so the migration only needs to record that it happened.
    if (document.schema_version == 1) {
        for (RegistryRule& rule : document.rules) {
            if (rule.origin_detail.empty()) {
                rule.origin_detail = "migrated from schema_version 1";
            }
            rule.rule_version = 1;
        }
        document.schema_version = 2;
        warnings.push_back("registry migrated from schema_version 1 to 2 (in memory; "
                           "run 'hyperion syscall-registry --write' to persist)");
        return true;
    }
    error = "no migration path from schema_version " + std::to_string(document.schema_version);
    return false;
}

void merge_default_rules(RegistryDocument& document, const RegistryDocument& defaults,
                         std::vector<std::string>& warnings) {
    for (const RegistryRule& candidate : defaults.rules) {
        auto existing = std::find_if(document.rules.begin(), document.rules.end(),
                                     [&](const RegistryRule& r) { return r.id == candidate.id; });
        if (existing == document.rules.end()) {
            document.rules.push_back(candidate);
            continue;
        }
        if (existing->origin == RuleOrigin::kUser) {
            warnings.push_back("default rule '" + candidate.id +
                               "' not applied: a user rule with that id is present");
            continue;
        }
        if (candidate.rule_version > existing->rule_version) {
            const std::string id = existing->id;
            const int version = existing->rule_version;
            *existing = candidate;
            warnings.push_back("default rule '" + id + "' updated from version " +
                               std::to_string(version) + " to " +
                               std::to_string(candidate.rule_version));
        }
    }
}

RegistryReport load_registry(const RegistryOptions& options, RegistryDocument& out_document) {
    RegistryReport report;
    const RegistryPathInfo info = resolve_registry_path(options);
    report.path = info.path;
    report.path_source = info.source;
    out_document = default_registry_document();

    if (info.path.empty()) {
        report.status = RegistryReport::Status::kNoLocation;
        report.message = info.reason;
        report.rules_loaded = out_document.rules.size();
        return report;
    }

    std::string text;
    std::string read_error;
    if (!read_text_file(info.path, text, read_error)) {
        const bool missing = (::access(info.path.c_str(), F_OK) != 0 && errno == ENOENT);
        if (missing) {
            report.status = RegistryReport::Status::kNotFound;
            report.message = "no registry at " + info.path + "; using built-in rules";
            if (options.create_if_missing) {
                RegistryDocument seed = default_registry_document();
                seed.created_unix = now_unix();
                seed.updated_unix = seed.created_unix;
                seed.generated_by = "hyperion (first run)";
                std::string write_error;
                if (write_registry_atomic(info.path, seed, write_error)) {
                    report.created = true;
                    report.status = RegistryReport::Status::kOk;
                    report.message = "created default registry at " + info.path;
                    out_document = seed;
                } else {
                    report.warnings.push_back("could not create " + info.path + ": " + write_error);
                }
            }
            report.rules_loaded = out_document.rules.size();
            return report;
        }
        report.status = RegistryReport::Status::kUnreadable;
        report.message = "cannot read " + info.path + ": " + read_error + "; using built-in rules";
        report.rules_loaded = out_document.rules.size();
        return report;
    }

    json::Value root;
    json::ParseError parse_error;
    if (!json::parse(text, root, parse_error)) {
        report.status = RegistryReport::Status::kInvalid;
        report.errors.push_back(parse_error.format(info.path));
        report.message = "registry is not valid JSON; built-in rules are in service and the "
                         "file was left untouched";
        report.rules_loaded = out_document.rules.size();
        return report;
    }

    std::vector<std::string> errors;
    RegistryDocument parsed = document_from_json(root, errors, report.warnings);
    if (!errors.empty()) {
        report.status = RegistryReport::Status::kInvalid;
        report.errors = errors;
        report.message = "registry failed schema validation; built-in rules are in service";
        report.rules_loaded = out_document.rules.size();
        return report;
    }
    if (parsed.guest_arch != "x86_64") {
        report.status = RegistryReport::Status::kInvalid;
        report.errors.push_back("document: guest_arch must be \"x86_64\" (found \"" +
                                parsed.guest_arch + "\")");
        report.message = "registry declares a guest architecture HYPERION cannot run";
        report.rules_loaded = out_document.rules.size();
        return report;
    }

    std::string migrate_error;
    if (!migrate_document(parsed, report.warnings, migrate_error)) {
        report.status = RegistryReport::Status::kInvalid;
        report.errors.push_back(migrate_error);
        report.message = "registry schema could not be migrated; built-in rules are in service";
        report.rules_loaded = out_document.rules.size();
        return report;
    }
    report.migrated = parsed.schema_version != kRegistrySchemaVersion;

    // Ship-with defaults are merged by stable id: a user edit to a default rule
    // survives an upgrade, and a deleted default comes back.
    merge_default_rules(parsed, default_registry_document(), report.warnings);
    if (parsed.created_unix == 0) {
        parsed.created_unix = now_unix();
    }
    out_document = std::move(parsed);
    report.status = RegistryReport::Status::kOk;
    report.message =
        "loaded " + std::to_string(out_document.rules.size()) + " rule(s) from " + info.path;
    report.rules_loaded = out_document.rules.size();
    for (const RegistryRule& rule : out_document.rules) {
        if (rule.origin == RuleOrigin::kUser) {
            ++report.user_rules;
        } else {
            ++report.package_rules;
        }
    }
    return report;
}

bool write_registry_atomic(const std::string& path, const RegistryDocument& document,
                           std::string& error) {
    if (path.empty()) {
        error = "no registry path";
        return false;
    }
    // Keep the previous contents recoverable before replacing them. If the
    // backup cannot be written, do not replace: losing the only copy of a user's
    // rules to a failed backup would be worse than refusing the write.
    std::string existing;
    std::string read_error;
    if (read_text_file(path, existing, read_error)) {
        const std::string backup = path + ".bak";
        std::string backup_error;
        if (!write_file_atomic_in_dir(backup, existing, backup_error)) {
            error = "cannot write backup '" + backup + "': " + backup_error;
            return false;
        }
    }
    return write_file_atomic_in_dir(path, document_to_text(document), error);
}

std::string document_to_text(const RegistryDocument& document) {
    return json::serialize(document_to_json(document));
}

} // namespace hyperion
