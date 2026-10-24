// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// HYPERION command-line entry point.
//
// Supports raw x86-64 code blobs as well as static (`ET_EXEC` / `ET_DYN`) and
// dynamically linked (`PT_INTERP` e.g. `ld-linux-x86-64.so.2`) Linux x86-64
// ELF64 executables across both execution tiers:
//   - Tier 0: Fast decoded-ICache + SoftTlb reference interpreter
//   - Tier 1: Full-system AArch64 + 128-bit ARMv8 NEON + IBTC JIT
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <unistd.h>

#include "hyperion/address_space.hpp"
#include "hyperion/elf.hpp"
#include "hyperion/elf_loader.hpp"
#include "hyperion/exec_mem.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/host_probe.hpp"
#include "hyperion/host_syscalls.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/sigsys_diagnostics.hpp"
#include "hyperion/syscall.hpp"
#include "hyperion/syscall_dispatch.hpp"
#include "hyperion/syscall_registry.hpp"
#include "hyperion/translation_cache.hpp"
#include "hyperion/version.hpp"

extern char** environ;

namespace {

constexpr std::uint64_t kLoadBase = 0x400000;
constexpr std::size_t kMemSize = 4096;
constexpr std::uint64_t kDefaultBlobMaxSteps = 1'000'000ULL;
constexpr std::uint64_t kDefaultElfMaxSteps = 1'000'000'000ULL;

void print_usage(const char* argv0) {
    std::printf(
        "hyperion - x86-64 to AArch64 user-mode dynamic binary translator\n"
        "\n"
        "usage: %s <command> [options] <file> [guest-args...]\n"
        "\n"
        "commands:\n"
        "  version      print version and build identity\n"
        "  probe        probe and report host capabilities\n"
        "  info <elf>   describe a static or dynamic ELF64 executable\n"
        "  run <file>   execute a raw x86-64 blob or Linux ELF64 executable\n"
        "  syscall-registry  inspect or validate the persistent syscall registry\n"
        "  help         show this message\n"
        "\n"
        "run options (must appear before the program path):\n"
        "  --tier=0|1       0 = reference interpreter, 1 = AArch64+NEON JIT (default 1)\n"
        "  --max-steps=N    guest instruction limit\n"
        "  --sysroot=DIR    x86-64 rootfs prefix for PT_INTERP / shared libraries\n"
        "  --quiet          suppress translator banner/summary (pure guest stdout + exit code)\n"
        "  --dump           print final guest register state\n"
        "  --no-syscall-registry  ignore the persistent syscall registry\n"
        "  --syscall-registry=FILE  use FILE instead of the default registry path\n"
        "\n"
        "Everything after the program path is passed to the guest unchanged, so\n"
        "guest options that start with '-' work without escaping:\n"
        "  %s run busybox-x86_64 sh -c 'printf \"9\\n2\\n\" | sort -n'\n"
        "\n"
        "The process exit status is the guest's own exit status.\n"
        "\n"
        "environment:\n"
        "  HYPERION_SYSROOT          default for --sysroot\n"
        "  HYPERION_TRACE_SYSCALLS=1 log every guest syscall and its result to stderr\n"
        "  HYPERION_SYSCALL_STATS=1  print syscall dispatch aggregates on exit\n"
        "  HYPERION_SYSCALL_REGISTRY override the persistent registry path\n"
        "  HYPERION_NO_SYSCALL_REGISTRY=1  disable the persistent registry\n",
        argv0, argv0);
}

int cmd_probe() {
    const hyperion::HostInfo info = hyperion::probe_host();
    std::printf("%s", hyperion::format_host_info(info).c_str());
    // Which host syscalls HYPERION is willing to issue, and why. On Android this
    // is what decides whether statx/memfd_create are used directly or
    // synthesized; see include/hyperion/host_syscalls.hpp.
    const int api = hyperion::host_android_api_level();
    std::printf("android api:     %d%s\n", api, api == 0 ? " (not Android, or unreadable)" : "");
    std::printf("statx(2):        %s\n", hyperion::host_statx_syscall_usable()
                                             ? "used directly"
                                             : "not issued; statx() synthesized from fstatat(2)");
    std::printf("memfd_create(2): %s\n", hyperion::host_memfd_create_syscall_usable()
                                             ? "used directly"
                                             : "not issued; O_TMPFILE fallback or -ENOSYS");
    if (hyperion::host_portable_syscalls_forced()) {
        std::printf("                 (HYPERION_PORTABLE_SYSCALLS is set)\n");
    }
    return 0;
}

bool read_file(const char* path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        std::fprintf(stderr, "hyperion: cannot open '%s'\n", path);
        return false;
    }
    std::uint8_t buffer[8192];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
        out.insert(out.end(), buffer, buffer + n);
    }
    std::fclose(f);
    return true;
}

int cmd_info(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "hyperion: info needs a file argument\n");
        return 2;
    }
    std::vector<std::uint8_t> file;
    if (!read_file(argv[2], file)) {
        return 2;
    }
    hyperion::ElfError err = hyperion::ElfError::kNone;
    auto image = hyperion::parse_elf64(file.data(), file.size(), err);
    if (!image) {
        std::fprintf(stderr, "hyperion: '%s': %s\n", argv[2], hyperion::to_string(err));
        return 1;
    }
    std::printf("file:        %s\n", argv[2]);
    std::printf("type:        ELF64 x86-64 %s\n",
                image->is_static() ? "static executable" : "dynamically linked executable");
    if (!image->is_static()) {
        std::printf("interpreter: %s\n", image->interpreter.c_str());
    }
    std::printf("entry:       0x%llx\n", static_cast<unsigned long long>(image->entry));
    std::printf("segments:    %zu PT_LOAD\n", image->segments.size());
    for (const hyperion::ElfSegment& s : image->segments) {
        const std::string perms = std::string(s.readable() ? "r" : "-") +
                                  (s.writable() ? "w" : "-") + (s.executable() ? "x" : "-");
        std::printf("  %-10s vaddr=0x%-10llx memsz=0x%-8llx filesz=0x%llx\n", perms.c_str(),
                    static_cast<unsigned long long>(s.vaddr),
                    static_cast<unsigned long long>(s.memsz),
                    static_cast<unsigned long long>(s.filesz));
    }
    std::printf("image end:   0x%llx\n", static_cast<unsigned long long>(image->image_end()));
    return 0;
}

int run_blob(const std::vector<std::uint8_t>& code, int tier, std::uint64_t max_steps, bool quiet,
             bool dump) {
    if (code.size() > kMemSize) {
        std::fprintf(stderr, "hyperion: blob is larger than the %zu-byte load window\n", kMemSize);
        return 2;
    }
    hyperion::FlatMemory memory(kLoadBase, kMemSize);
    std::memcpy(memory.data(), code.data(), code.size());

    hyperion::GuestState state;
    state.rip = kLoadBase;

    if (tier == 1 && !hyperion::ExecMem::executable_supported()) {
        std::fprintf(stderr, "hyperion: this host cannot execute generated AArch64 code; "
                             "falling back to the reference interpreter (tier 0)\n");
        tier = 0;
    }

    hyperion::RunResult result;
    if (tier == 0) {
        result = hyperion::run_program(memory, state, max_steps);
    } else {
        hyperion::TranslationCache cache;
        result = cache.run(memory, state, max_steps);
        if (!quiet) {
            std::printf("tier 1: %zu basic block(s) translated\n", cache.block_count());
        }
    }

    if (!quiet) {
        std::printf("status: %s\n", hyperion::to_string(result.status));
        std::printf("steps:  %llu\n", static_cast<unsigned long long>(result.steps));
        if (!result.message.empty()) {
            std::printf("detail: %s\n", result.message.c_str());
        }
    }
    if (dump) {
        std::printf("%s", hyperion::to_string(result.state).c_str());
    }
    return result.status == hyperion::RunStatus::kOk ? 0 : 1;
}

// Options that control the persistent syscall compatibility registry.
struct RegistryCli {
    bool enabled = true;
    std::string path; // empty: use the documented default resolution
};

bool env_flag_enabled(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

int run_elf(const std::vector<std::uint8_t>& file, const std::vector<std::string>& guest_argv,
            const std::string& guest_image_path, const std::string& sysroot, int tier,
            std::uint64_t max_steps, bool quiet, bool dump, const RegistryCli& registry) {
    // Disjoint 256 MiB heap at 0x20000000 and 1 GiB per-mapping ceiling at 0x7f0000000000.
    hyperion::GuestAddressSpace space(0x7f0000000000ULL, 0x20000000ULL, 256ULL << 20);
    hyperion::GuestState state;

    std::vector<std::string> envp;
    if (environ != nullptr) {
        for (char** ep = environ; *ep != nullptr; ++ep) {
            // Force C locale when running foreign arch binaries without matching locale-archive.
            if (std::strncmp(*ep, "LC_", 3) == 0 || std::strncmp(*ep, "LANG=", 5) == 0) {
                continue;
            }
            envp.emplace_back(*ep);
        }
    }
    envp.emplace_back("LC_ALL=C");

    // AT_EXECFN and /proc/self/exe both have to name the executable by an
    // absolute path, the way the kernel records it. The user may have passed a
    // relative one, so resolve it here rather than letting a relative argv[0]
    // reach the guest's auxiliary vector.
    std::string absolute_image = guest_image_path;
    if (!absolute_image.empty() && absolute_image.front() != '/') {
        std::vector<char> cwd(4096);
        if (::getcwd(cwd.data(), cwd.size()) != nullptr) {
            absolute_image = std::string(cwd.data()) + "/" + absolute_image;
        }
    }

    const hyperion::LoadedElf loaded =
        hyperion::load_linux_elf_process(file.data(), file.size(), space, state, guest_argv, envp,
                                         sysroot, 8ULL << 20, absolute_image);
    if (!loaded.ok()) {
        std::fprintf(stderr, "hyperion: cannot load image: %s\n",
                     hyperion::to_string(loaded.error));
        return 1;
    }
    if (!quiet) {
        std::printf("loaded ELF at entry 0x%llx (rsp=0x%llx)\n",
                    static_cast<unsigned long long>(loaded.entry),
                    static_cast<unsigned long long>(state.reg(hyperion::Gpr::kRsp)));
    }

    if (tier == 1 && !hyperion::ExecMem::executable_supported()) {
        std::fprintf(stderr, "hyperion: this host cannot execute generated AArch64 code; "
                             "falling back to the reference interpreter (tier 0)\n");
        tier = 0;
    }

    hyperion::MinimalSyscallHandler syscalls({}, &space);
    syscalls.enable_linux_compat(sysroot);
    // Tell the handler which image the guest came from. A guest is running
    // inside this host process, so the host's /proc/self/exe names hyperion
    // itself; without this, a guest that re-executes through that path (BusyBox
    // does, to run an applet) would exec the translator instead of itself.
    syscalls.set_guest_image_path(absolute_image);

    // The persistent registry is loaded exactly once, here, before the guest
    // starts. Nothing is read from the filesystem once execution begins.
    std::string registry_message;
    if (registry.enabled) {
        hyperion::RegistryOptions options;
        if (!registry.path.empty()) {
            options.use_environment = false;
            options.explicit_path = registry.path;
        }
        const int registry_status = syscalls.reload_syscall_registry(options, registry_message);
        if (!quiet && registry_status != 0) {
            std::fprintf(stderr, "hyperion: syscall registry: %s\n", registry_message.c_str());
        }
    }

    hyperion::RunResult result;
    if (tier == 0) {
        result = hyperion::run_program(space, state, max_steps, &syscalls);
    } else {
        hyperion::TranslationCache cache;
        result = cache.run(space, state, max_steps, &syscalls);
        if (!quiet) {
            std::printf("tier 1: %zu basic block(s) translated\n", cache.block_count());
        }
    }

    if (!quiet) {
        std::printf("status: %s\n", hyperion::to_string(result.status));
        std::printf("steps:  %llu\n", static_cast<unsigned long long>(result.steps));
        if (result.unimplemented_syscalls != 0) {
            // This figure is the number of *events*, not the number of distinct
            // syscall numbers: every guest `syscall` instruction whose handler
            // returned -ENOSYS increments it by one. The breakdown below names
            // the distinct numbers involved.
            std::printf("unimplemented syscalls: %llu\n",
                        static_cast<unsigned long long>(result.unimplemented_syscalls));
        }
        if (result.unimplemented_syscalls != 0 || env_flag_enabled("HYPERION_SYSCALL_STATS")) {
            std::printf("%s", syscalls.dispatcher().summary().c_str());
        }
        if (!result.message.empty()) {
            std::printf("detail: %s\n", result.message.c_str());
        }
    }
    if (dump) {
        std::printf("%s", hyperion::to_string(result.state).c_str());
    }
    if (result.status != hyperion::RunStatus::kOk) {
        return 1;
    }
    // The guest's own exit status is the process status, whether or not the
    // banner was suppressed. This used to be honoured only under --quiet, which
    // made `hyperion run` exit 0 for a guest that had failed -- a CI gate built
    // on the exit status would then have been told a broken run succeeded.
    if (result.exited_via_syscall) {
        return static_cast<int>(result.exit_code & 0xFF);
    }
    return 0;
}

int cmd_run(int argc, char** argv) {
    const char* path = nullptr;
    int tier = 1;
    bool explicit_steps = false;
    std::uint64_t max_steps = kDefaultBlobMaxSteps;
    bool quiet = false;
    bool dump = false;
    RegistryCli registry;
    if (env_flag_enabled("HYPERION_NO_SYSCALL_REGISTRY")) {
        registry.enabled = false;
    }
    std::string sysroot;
    if (const char* env_sr = std::getenv("HYPERION_SYSROOT"); env_sr != nullptr) {
        sysroot = env_sr;
    }
    std::vector<std::string> guest_argv;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--") {
            for (int j = i + 1; j < argc; ++j) {
                guest_argv.emplace_back(argv[j]);
            }
            break;
        }
        // Hyperion's own options are recognised in any position, so
        // `run prog --dump` keeps working. Any *other* argument that starts with
        // a dash is a guest argument once the program path has been seen --
        // without that rule `run busybox sh -c "a | b"` fails, because hyperion
        // would eat the guest's `-c` as an unknown option of its own. Before the
        // path, an unrecognised dash is still an error, which catches typos.
        // `--` ends option parsing entirely for callers who want to pass a
        // literal `--dump` to a guest.
        const bool is_hyperion_option =
            arg.rfind("--tier=", 0) == 0 || arg.rfind("--max-steps=", 0) == 0 ||
            arg.rfind("--sysroot=", 0) == 0 || arg == "--quiet" || arg == "-q" || arg == "--dump" ||
            arg == "--no-syscall-registry" || arg.rfind("--syscall-registry=", 0) == 0;

        if (is_hyperion_option) {
            if (arg.rfind("--tier=", 0) == 0) {
                tier = std::atoi(arg.c_str() + 7);
            } else if (arg.rfind("--max-steps=", 0) == 0) {
                max_steps = std::strtoull(arg.c_str() + 12, nullptr, 10);
                explicit_steps = true;
            } else if (arg.rfind("--sysroot=", 0) == 0) {
                sysroot = arg.substr(10);
            } else if (arg == "--quiet" || arg == "-q") {
                quiet = true;
            } else if (arg == "--no-syscall-registry") {
                registry.enabled = false;
            } else if (arg.rfind("--syscall-registry=", 0) == 0) {
                registry.path = arg.substr(19);
                registry.enabled = true;
            } else {
                dump = true;
            }
            continue;
        }
        if (path != nullptr) {
            guest_argv.emplace_back(arg);
        } else if (arg.rfind('-', 0) == 0) {
            std::fprintf(stderr, "hyperion: unknown run option '%s'\n", arg.c_str());
            return 2;
        } else {
            path = argv[i];
        }
    }

    if (path == nullptr) {
        std::fprintf(stderr, "hyperion: run needs a file argument\n");
        return 2;
    }
    if (tier != 0 && tier != 1) {
        std::fprintf(stderr, "hyperion: --tier must be 0 or 1\n");
        return 2;
    }

    std::vector<std::uint8_t> code;
    if (!read_file(path, code) || code.empty()) {
        std::fprintf(stderr, "hyperion: '%s' is empty or unreadable\n", path);
        return 2;
    }

    if (hyperion::looks_like_elf(code.data(), code.size())) {
        std::vector<std::string> full_argv;
        full_argv.emplace_back(path);
        full_argv.insert(full_argv.end(), guest_argv.begin(), guest_argv.end());
        return run_elf(code, full_argv, path, sysroot, tier,
                       explicit_steps ? max_steps : kDefaultElfMaxSteps, quiet, dump, registry);
    }
    return run_blob(code, tier, max_steps, quiet, dump);
}

// `hyperion syscall-registry` - inspect, validate and export the persistent
// syscall compatibility registry. Deliberately read-only unless --write is
// given: inspecting the registry must never mutate it.
// Exit status of `syscall-registry --validate`:
//   0  the file parsed and every rule in it is in effect
//   1  the file could not be used at all (missing, unreadable, bad JSON, newer
//      schema); HYPERION falls back to its built-in rules
//   3  the file parsed, but at least one rule was rejected and is NOT in effect
//      - a user asking "did my rule take?" must not be told "yes" here
int validate_exit(int status, std::size_t rejected) {
    if (status != 0) {
        return 1;
    }
    return rejected == 0 ? 0 : 3;
}

int cmd_syscall_registry(int argc, char** argv) {
    hyperion::RegistryOptions options;
    bool as_json = false;
    bool list = false;
    bool validate = false;
    bool export_defaults = false;
    bool write = false;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") {
            as_json = true;
        } else if (arg == "--list") {
            list = true;
        } else if (arg == "--validate") {
            validate = true;
        } else if (arg == "--export-defaults") {
            export_defaults = true;
        } else if (arg == "--write") {
            write = true;
        } else if (arg.rfind("--registry=", 0) == 0) {
            options.use_environment = false;
            options.explicit_path = arg.substr(11);
        } else {
            std::fprintf(stderr, "hyperion: unknown syscall-registry option '%s'\n", arg.c_str());
            return 2;
        }
    }

    if (export_defaults) {
        std::printf("%s",
                    hyperion::document_to_text(hyperion::default_registry_document()).c_str());
        return 0;
    }

    // Compiling through a handler reuses exactly the validation path that the
    // guest run uses, so `--validate` cannot disagree with `run`.
    hyperion::MinimalSyscallHandler handler({}, nullptr);
    std::string message;
    const int status = handler.reload_syscall_registry(options, message);
    const hyperion::SyscallDispatcher& dispatcher = handler.dispatcher();
    const hyperion::CompiledRules* rules = dispatcher.rules();

    // Rules the compile pipeline refused are listed by the dispatcher; pull them
    // out so the exit status can reflect them.
    std::vector<std::string> rejected_rules;
    for (const std::string& diagnostic : dispatcher.diagnostics()) {
        if (diagnostic.rfind("rejected:", 0) == 0) {
            rejected_rules.push_back(diagnostic.substr(std::strlen("rejected: ")));
        }
    }

    if (as_json) {
        // A machine-readable view of what will actually happen, not of the file's
        // text: the effective rule set (built-ins merged with the user's file),
        // the resolved path, and every rule that was rejected. A tool that
        // reported only the file's own contents could not answer the question a
        // user actually has - "is my rule in effect?".
        const hyperion::RegistryPathInfo info = hyperion::resolve_registry_path(options);
        std::string report = "{\n";
        report += "  \"status\": ";
        report += hyperion::json::quote(status == 0 ? "ok" : "built-in-rules-only");
        report += ",\n  \"message\": ";
        report += hyperion::json::quote(message);
        report += ",\n  \"path\": ";
        report += hyperion::json::quote(info.path);
        report += ",\n  \"path_source\": ";
        report += hyperion::json::quote(info.source);
        report += ",\n  \"path_reason\": ";
        report += hyperion::json::quote(info.reason);
        report += ",\n  \"schema_version\": ";
        report += std::to_string(hyperion::kRegistrySchemaVersion);
        report += ",\n  \"counts\": {\"adapter\": ";
        report += std::to_string(rules != nullptr ? rules->adapter_rules : 0);
        report += ", \"alias\": ";
        report += std::to_string(rules != nullptr ? rules->alias_rules : 0);
        report += ", \"rejected\": " + std::to_string(rejected_rules.size());
        report += "},\n  \"rules\": [";
        if (rules != nullptr) {
            bool first = true;
            for (const hyperion::RegistryRule& rule : rules->rules) {
                report += first ? "\n" : ",\n";
                first = false;
                report += "    {\"id\": " + hyperion::json::quote(rule.id);
                report += ", \"name\": " + hyperion::json::quote(rule.name);
                report += ", \"syscall\": " + std::to_string(rule.syscall);
                report += ", \"implementation\": ";
                report += hyperion::json::quote(hyperion::to_string(rule.implementation));
                report += ", \"handler\": " + hyperion::json::quote(rule.handler);
                report += ", \"origin\": ";
                report += hyperion::json::quote(hyperion::to_string(rule.origin));
                report += ", \"rule_version\": " + std::to_string(rule.rule_version);
                report += ", \"enabled\": ";
                report += rule.enabled ? "true" : "false";
                report += "}";
            }
        }
        report += "\n  ],\n  \"rejected\": [";
        for (std::size_t i = 0; i < rejected_rules.size(); ++i) {
            report += i == 0 ? "\n    " : ",\n    ";
            report += hyperion::json::quote(rejected_rules[i]);
        }
        report += rejected_rules.empty() ? "],\n" : "\n  ],\n";
        report += "  \"diagnostics\": [";
        const std::vector<std::string>& diagnostics = dispatcher.diagnostics();
        for (std::size_t i = 0; i < diagnostics.size(); ++i) {
            report += i == 0 ? "\n    " : ",\n    ";
            report += hyperion::json::quote(diagnostics[i]);
        }
        report += diagnostics.empty() ? "]\n}\n" : "\n  ]\n}\n";
        std::printf("%s", report.c_str());
        return validate_exit(status, rejected_rules.size());
    }

    const hyperion::RegistryPathInfo info = hyperion::resolve_registry_path(options);
    std::printf("registry path  : %s\n", info.path.empty() ? "(none)" : info.path.c_str());
    std::printf("resolved from  : %s (%s)\n", info.source.c_str(), info.reason.c_str());
    std::printf("status         : %s\n", status == 0 ? "ok" : "built-in rules only");
    std::printf("detail         : %s\n", message.c_str());
    if (rules != nullptr) {
        std::printf("compiled rules : %llu adapter, %llu alias\n",
                    static_cast<unsigned long long>(rules->adapter_rules),
                    static_cast<unsigned long long>(rules->alias_rules));
    }
    for (const std::string& diagnostic : dispatcher.diagnostics()) {
        std::printf("  %s\n", diagnostic.c_str());
    }

    if (list && rules != nullptr) {
        std::printf("\n%-6s %-24s %-16s %-28s %s\n", "syscall", "name", "implementation", "handler",
                    "origin");
        for (const hyperion::RegistryRule& rule : rules->rules) {
            std::printf("%-6llu %-24s %-16s %-28s %s\n",
                        static_cast<unsigned long long>(rule.syscall), rule.name.c_str(),
                        hyperion::to_string(rule.implementation),
                        rule.handler.empty() ? "-" : rule.handler.c_str(),
                        hyperion::to_string(rule.origin));
        }
    }

    if (write) {
        hyperion::RegistryDocument document;
        hyperion::RegistryReport report = hyperion::load_registry(options, document);
        std::string write_error;
        if (!report.ok()) {
            std::fprintf(stderr, "hyperion: refusing to write: %s\n", report.message.c_str());
            return 1;
        }
        document.updated_unix = static_cast<std::int64_t>(::time(nullptr));
        if (!hyperion::write_registry_atomic(info.path, document, write_error)) {
            std::fprintf(stderr, "hyperion: cannot write registry: %s\n", write_error.c_str());
            return 1;
        }
        std::printf("wrote          : %s (previous contents kept as %s.bak)\n", info.path.c_str(),
                    info.path.c_str());
    }

    if (!validate) {
        return 0;
    }
    return validate_exit(status, rejected_rules.size());
}

} // namespace

int main(int argc, char** argv) {
    // Report (but never swallow) a SIGSYS from the platform's seccomp policy:
    // see hyperion/sigsys_diagnostics.hpp. The exit status is unchanged.
    hyperion::install_sigsys_diagnostics("hyperion");
    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }

    const std::string command = argv[1];
    if (command == "version" || command == "--version" || command == "-V") {
        std::printf("%s\n", hyperion::build_summary().c_str());
        return 0;
    }
    if (command == "probe") {
        return cmd_probe();
    }
    if (command == "info") {
        return cmd_info(argc, argv);
    }
    if (command == "run") {
        return cmd_run(argc, argv);
    }
    if (command == "syscall-registry") {
        return cmd_syscall_registry(argc, argv);
    }
    if (command == "help" || command == "--help" || command == "-h") {
        print_usage(argv[0]);
        return 0;
    }

    std::fprintf(stderr, "hyperion: unknown command '%s'\n\n", command.c_str());
    print_usage(argv[0]);
    return 2;
}
