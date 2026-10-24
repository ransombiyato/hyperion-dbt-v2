// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Persistent, external syscall compatibility registry.
//
// The registry is *configuration*: it selects from the compiled-in handler
// whitelist (see syscall_dispatch.hpp). A JSON rule cannot execute a shell
// command, load a shared library, run a host program or call an arbitrary host
// syscall number, and a rule whose handler does not exist is rejected rather
// than honoured. That property is enforced in syscall_dispatch.cpp, at load
// time, before any rule reaches the dispatch table.
//
// Location and persistence
// ------------------------
// The file lives outside the HYPERION installation so it survives replacing the
// binary, updating the source tree or reinstalling. Resolution order (first
// usable wins):
//
//   1. $HYPERION_SYSCALL_REGISTRY          explicit absolute path override
//   2. $XDG_DATA_HOME/hyperion/syscalls/registry.json
//   3. $HOME/.local/share/hyperion/syscalls/registry.json
//   4. (no home directory)                 no persistence; built-in rules only
//
// On Android/Termux $HOME is /data/data/com.termux/files/home, so rule 3 yields
// the app-private, backed-up location the platform expects. On a conventional
// Linux host the same rule gives the XDG default when XDG_DATA_HOME is unset.
// Nothing is ever written to /usr, /etc or any other system directory.
//
// Writes are atomic (temporary file in the same directory, fsync, rename,
// fsync of the directory) with mode 0600, and the previous contents are kept as
// registry.json.bak before replacement. An interrupted write therefore cannot
// produce a truncated registry.
#ifndef HYPERION_SYSCALL_REGISTRY_HPP
#define HYPERION_SYSCALL_REGISTRY_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "hyperion/json.hpp"

namespace hyperion {

// Current schema. Version 1 is the same document without `origin`,
// `rule_version` and `arg_source`; it is migrated on load and rewritten in the
// current version only when the user asks for a write.
inline constexpr int kRegistrySchemaVersion = 2;
inline constexpr int kRegistryMinSchemaVersion = 1;

// The only return-value convention HYPERION speaks: a negative errno in rax.
inline constexpr const char* kRegistryReturnTransformErrno = "errno_negative";

enum class RuleImplementation : std::uint8_t {
    kBuiltinAdapter, // call a compiled-in adapter named by `handler`
    kGuestAlias,     // rewrite registers and re-dispatch an existing guest syscall
    kDisable,        // explicitly keep the syscall unsupported (-ENOSYS)
};

const char* to_string(RuleImplementation impl);
bool parse_rule_implementation(const std::string& text, RuleImplementation& out);

enum class RuleOrigin : std::uint8_t {
    kPackage, // shipped by HYPERION (the versioned default set)
    kUser,    // written by a person or a tool acting for one
};

const char* to_string(RuleOrigin origin);
bool parse_rule_origin(const std::string& text, RuleOrigin& out);

// One argument of an alias target: either a constant or "the source syscall's
// argument N".
struct AliasArg {
    enum class Kind : std::uint8_t { kConst, kArg };
    Kind kind = Kind::kConst;
    std::int64_t value = 0;
};

// One compatibility rule.
struct RegistryRule {
    // Stable identity used for merging defaults with user edits. Never derived
    // from the syscall number, so a rule can be retargeted without becoming a
    // different rule.
    std::string id;
    std::string name;             // symbolic guest name, e.g. "chdir"
    std::uint64_t syscall = 0;    // x86-64 guest syscall number

    RuleImplementation implementation = RuleImplementation::kBuiltinAdapter;
    std::string handler;              // adapter id when implementation == kBuiltinAdapter
    std::uint64_t target_syscall = 0; // target when implementation == kGuestAlias
    std::vector<AliasArg> arg_source; // up to 6 entries when implementation == kGuestAlias

    // Declarative ABI description. Validated against the selected adapter's own
    // declaration: a rule may not claim a transformation that is not performed,
    // nor omit one that is.
    std::vector<std::string> transforms;
    std::string return_transform = kRegistryReturnTransformErrno;

    // Preconditions. Currently: "linux_compat" (needs enable_linux_compat), and
    // "host_arch:aarch64" / "host_arch:x86_64". Serialised under the JSON key
    // "requires"; the C++ member is spelled `requirements` because `requires`
    // is a C++20 keyword.
    std::vector<std::string> requirements;

    int rule_version = 1;
    RuleOrigin origin = RuleOrigin::kUser;
    std::string origin_detail; // free-form provenance: tool, version, date
    bool enabled = true;
    std::string notes;
};

struct RegistryDocument {
    int schema_version = kRegistrySchemaVersion;
    std::string guest_arch = "x86_64";
    std::string host_arch; // "" = host-agnostic
    std::string generated_by;
    std::string comment;
    std::int64_t created_unix = 0;
    std::int64_t updated_unix = 0;
    std::vector<RegistryRule> rules;
};

// Where the registry lives and why.
struct RegistryPathInfo {
    std::string path;    // empty when no persistent location is available
    std::string source;  // "override", "xdg", "home", or "none"
    std::string reason;  // human-readable explanation for diagnostics
};

struct RegistryOptions {
    // Honour $HYPERION_SYSCALL_REGISTRY. A CLI flag sets it to false and fills
    // `explicit_path` instead, so an explicit command-line choice always wins.
    bool use_environment = true;
    std::string explicit_path;
    // Create and seed the default registry when the file does not exist.
    bool create_if_missing = true;
    // Fall back to built-in rules when the file is missing, unreadable, corrupt
    // or from an unsupported schema version. HYPERION always starts.
    bool fallback_to_defaults = true;
};

struct RegistryReport {
    enum class Status : std::uint8_t {
        kOk,          // loaded (possibly after creating or migrating)
        kNotFound,    // no file; built-in defaults in service
        kUnreadable,  // exists but cannot be read; built-in defaults in service
        kInvalid,     // failed validation; built-in defaults in service
        kNoLocation,  // no writable persistent location; built-in defaults
        kNotWritten,  // the document was valid but persistence failed
    };

    Status status = Status::kNotFound;
    std::string path;
    std::string path_source;
    std::string message;
    // Fatal problems with the document as a whole.
    std::vector<std::string> errors;
    // Individual rules refused during validation; the rest still load.
    std::vector<std::string> rejected;
    // Non-fatal conditions worth reporting (migrations, ignored unknowns).
    std::vector<std::string> warnings;
    bool created = false;
    bool migrated = false;
    bool backup_written = false;
    std::uint64_t rules_loaded = 0;
    std::uint64_t user_rules = 0;
    std::uint64_t package_rules = 0;

    bool ok() const noexcept {
        return status == Status::kOk || status == Status::kNotFound ||
               status == Status::kNoLocation || status == Status::kUnreadable ||
               status == Status::kNotWritten;
    }
};

// Resolve the registry path without touching the filesystem.
RegistryPathInfo resolve_registry_path(const RegistryOptions& options);

// The versioned default document HYPERION ships. Every rule here selects an
// adapter that exists in this build; `validate_document` re-checks that.
RegistryDocument default_registry_document();

// Parse a document from JSON text. Structural problems (wrong types, missing
// required fields, unknown implementation names) become entries in `errors`;
// the parsed document is only meaningful when `errors` is empty.
RegistryDocument document_from_json(const json::Value& root, std::vector<std::string>& errors,
                                    std::vector<std::string>& warnings);

json::Value document_to_json(const RegistryDocument& document);

// Migrate `document` in place to kRegistrySchemaVersion. Returns false when the
// version is unknown or newer than this build supports (a newer file is never
// downgraded: it is reported and left untouched).
bool migrate_document(RegistryDocument& document, std::vector<std::string>& warnings,
                      std::string& error);

// Merge the shipped defaults into `document` by rule id:
//   - a default whose id is absent is appended;
//   - a default whose id is present as a package rule is replaced when the
//     default's rule_version is newer, otherwise left alone;
//   - a default whose id is present as a *user* rule is not applied: the user's
//     rule wins and is reported in `warnings`.
// No user rule is ever removed or reordered.
void merge_default_rules(RegistryDocument& document, const RegistryDocument& defaults,
                         std::vector<std::string>& warnings);

// Load (and, when required, create/migrate/merge) the registry. Never throws and
// never leaves HYPERION unable to run: on any failure the caller keeps the
// built-in rules.
RegistryReport load_registry(const RegistryOptions& options, RegistryDocument& out_document);

// Serialise `document` to `path` atomically, keeping the previous contents as
// `<path>.bak`. Creates the parent directory (0700) when needed. Returns false
// and fills `error` on failure; the existing file is then untouched.
bool write_registry_atomic(const std::string& path, const RegistryDocument& document,
                           std::string& error);

// Read a file into `out`, bounded by `max_bytes`.
bool read_text_file(const std::string& path, std::string& out, std::string& error,
                    std::size_t max_bytes = 16U * 1024U * 1024U);

// Serialise a document for display / `--export-defaults`.
std::string document_to_text(const RegistryDocument& document);

} // namespace hyperion

#endif // HYPERION_SYSCALL_REGISTRY_HPP
