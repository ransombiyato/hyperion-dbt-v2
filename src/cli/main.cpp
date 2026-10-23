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
#include <string>
#include <vector>

#include <unistd.h>

#include "hyperion/address_space.hpp"
#include "hyperion/elf.hpp"
#include "hyperion/elf_loader.hpp"
#include "hyperion/exec_mem.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/host_probe.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/syscall.hpp"
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
        "  help         show this message\n"
        "\n"
        "run options (must appear before the program path):\n"
        "  --tier=0|1       0 = reference interpreter, 1 = AArch64+NEON JIT (default 1)\n"
        "  --max-steps=N    guest instruction limit\n"
        "  --sysroot=DIR    x86-64 rootfs prefix for PT_INTERP / shared libraries\n"
        "  --quiet          suppress translator banner/summary (pure guest stdout + exit code)\n"
        "  --dump           print final guest register state\n"
        "\n"
        "Everything after the program path is passed to the guest unchanged, so\n"
        "guest options that start with '-' work without escaping:\n"
        "  %s run busybox-x86_64 sh -c 'printf \"9\\n2\\n\" | sort -n'\n"
        "\n"
        "The process exit status is the guest's own exit status.\n"
        "\n"
        "environment:\n"
        "  HYPERION_SYSROOT          default for --sysroot\n"
        "  HYPERION_TRACE_SYSCALLS=1 log every guest syscall and its result to stderr\n",
        argv0, argv0);
}

int cmd_probe() {
    const hyperion::HostInfo info = hyperion::probe_host();
    std::printf("%s", hyperion::format_host_info(info).c_str());
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

int run_elf(const std::vector<std::uint8_t>& file, const std::vector<std::string>& guest_argv,
            const std::string& guest_image_path, const std::string& sysroot, int tier,
            std::uint64_t max_steps, bool quiet, bool dump) {
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
            std::printf("unimplemented syscalls: %llu\n",
                        static_cast<unsigned long long>(result.unimplemented_syscalls));
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
            arg.rfind("--sysroot=", 0) == 0 || arg == "--quiet" || arg == "-q" || arg == "--dump";

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
                       explicit_steps ? max_steps : kDefaultElfMaxSteps, quiet, dump);
    }
    return run_blob(code, tier, max_steps, quiet, dump);
}

} // namespace

int main(int argc, char** argv) {
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
    if (command == "help" || command == "--help" || command == "-h") {
        print_usage(argv[0]);
        return 0;
    }

    std::fprintf(stderr, "hyperion: unknown command '%s'\n\n", command.c_str());
    print_usage(argv[0]);
    return 2;
}
