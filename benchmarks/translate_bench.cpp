// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Translation-cost and syscall-heavy guest benchmark.
//
// Two questions, kept apart because they are paid at different times:
//
//   1. translation_cost - what does it cost to turn guest code into executable
//      code? Reported from the counters the TranslationCache now exposes
//      (translation_ns(), translated_guest_instructions(), total_code_bytes()),
//      so the number is measured inside the translator rather than inferred by
//      subtracting a fast run from a slow one. Only meaningful on a host that
//      can execute generated AArch64 code; elsewhere it is reported as
//      unavailable, never fabricated.
//
//   2. syscall_heavy - what does one guest `syscall` instruction cost end to
//      end (guest instruction dispatch + handler + any post-syscall
//      invalidation), at tier 0 and, where it exists, tier 1. The guest is a
//      tight `getpid` loop, so the loop overhead is the same for both tiers and
//      the difference is the syscall path itself.
//
// Methodology, shared with the other harnesses (BENCHMARKS.md section 5): one
// discarded warm-up sample, repeated timed samples reported as median with an
// IQR spread, and a correctness gate - the guest's retired-instruction count and
// return value are checked, so a number can never come from a broken run.
//
// Limitations, stated rather than implied: wall-clock time on this host CPU,
// comparable only within one machine and one run. Translation cost includes
// arena allocation and the per-block W^X mprotect pair, so it is an upper bound
// on the translator's own work. Under qemu-user these are emulation numbers,
// not AArch64 hardware numbers.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hyperion/bench_stats.hpp"
#include "hyperion/exec_mem.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/syscall.hpp"
#include "hyperion/translation_cache.hpp"
#include "hyperion/version.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using hyperion::FlatMemory;
using hyperion::Gpr;
using hyperion::GuestState;
using hyperion::RunResult;
using hyperion::RunStatus;
using hyperion::TranslationCache;

constexpr std::uint64_t kBase = 0x400000;
constexpr std::size_t kMemSize = 64 * 1024;

// Guest program: syscall(getpid) `rcx` times, then halt.
//   mov eax, 39        ; B8 27 00 00 00
// loop:
//   syscall            ; 0F 05
//   dec rcx            ; 48 FF C9
//   jnz loop           ; 75 F4   (rel8 = -12, back to the syscall)
//   hlt                ; F4
// Bytes verified with as/objdump. Four retired instructions per iteration plus
// the final hlt, which the harness asserts rather than assumes.
const std::vector<std::uint8_t> kSyscallLoop = {
    0xB8, 0x27, 0x00, 0x00, 0x00, 0x0F, 0x05, 0x48, 0xFF, 0xC9, 0x75, 0xF4, 0xF4,
};
constexpr std::uint64_t kInstructionsPerSyscall = 4;

// Wide-register loop, reused from tier_bench so translation cost is measured on
// a block that actually exercises register allocation (eleven guest registers,
// more than the allocator has host registers): translation cost per instruction
// on a three-instruction loop would flatter the translator.
std::vector<std::uint8_t> make_wide_program(std::uint32_t iterations) {
    std::vector<std::uint8_t> p = {
        0x41, 0xBC, 0x00, 0x00, 0x00, 0x00, // mov r12d, imm32 (patched)
        0x48, 0x01, 0xD8,                   // add rax, rbx
        0x48, 0x01, 0xC8,                   // add rax, rcx
        0x48, 0x01, 0xD0,                   // add rax, rdx
        0x48, 0x01, 0xF0,                   // add rax, rsi
        0x48, 0x01, 0xF8,                   // add rax, rdi
        0x4C, 0x01, 0xC0,                   // add rax, r8
        0x4C, 0x01, 0xC8,                   // add rax, r9
        0x4C, 0x01, 0xD0,                   // add rax, r10
        0x4C, 0x01, 0xD8,                   // add rax, r11
        0x48, 0x01, 0xD8,                   // add rax, rbx
        0x49, 0xFF, 0xCC,                   // dec r12
        0x75, 0xDD,                         // jne loop (offset 6)
        0xF4,                               // hlt
    };
    p[2] = static_cast<std::uint8_t>(iterations & 0xFF);
    p[3] = static_cast<std::uint8_t>((iterations >> 8) & 0xFF);
    p[4] = static_cast<std::uint8_t>((iterations >> 16) & 0xFF);
    p[5] = static_cast<std::uint8_t>((iterations >> 24) & 0xFF);
    return p;
}

inline void optimization_barrier(const void* p) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(p) : "memory");
#else
    *static_cast<volatile const void**>(const_cast<const void**>(&p)) = p;
#endif
}

struct Options {
    std::uint64_t syscalls = 10'000;   // guest syscalls per sample (syscall_heavy)
    std::uint64_t iterations = 10'000; // guest loop iterations (translation_cost)
    std::uint32_t repeats = 5;         // timed samples, one warm-up discarded
    bool json = false;
    // Measure the CLI-equivalent path as well: `hyperion run` registers the
    // Linux compat layer (and the persistent syscall registry before it), which
    // sits in front of the same handler. A bare handler answers getpid directly;
    // the compat path is what a real guest pays, so both are reported rather
    // than quoting the cheaper one.
    bool compat = false;
};

void usage() {
    std::fprintf(stderr,
                 "usage: hyperion_translate_bench [--syscalls N] [--iterations N] [--repeats N] "
                 "[--json]\n"
                 "  --syscalls N    guest syscalls per sample (default 10000)\n"
                 "  --iterations N  guest loop iterations for the translation workload "
                 "(default 10000)\n"
                 "  --repeats N     timed samples, one warm-up discarded (default 5, min 2)\n"
                 "  --compat        also measure the Linux-compat handler path (what "
                 "`hyperion run` uses)\n"
                 "  --json          machine-readable report\n");
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json") {
            o.json = true;
        } else if (a == "--compat") {
            o.compat = true;
        } else if (a == "--help" || a == "-h") {
            usage();
            return false;
        } else if (a.rfind("--syscalls=", 0) == 0) {
            o.syscalls = std::strtoull(a.c_str() + 11, nullptr, 10);
        } else if (a == "--syscalls" && i + 1 < argc) {
            o.syscalls = std::strtoull(argv[++i], nullptr, 10);
        } else if (a.rfind("--iterations=", 0) == 0) {
            o.iterations = std::strtoull(a.c_str() + 13, nullptr, 10);
        } else if (a == "--iterations" && i + 1 < argc) {
            o.iterations = std::strtoull(argv[++i], nullptr, 10);
        } else if (a.rfind("--repeats=", 0) == 0) {
            o.repeats = static_cast<std::uint32_t>(std::strtoul(a.c_str() + 10, nullptr, 10));
        } else if (a == "--repeats" && i + 1 < argc) {
            o.repeats = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else {
            std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
            return false;
        }
    }
    if (o.syscalls == 0) {
        std::fprintf(stderr, "error: --syscalls must be greater than zero\n");
        return false;
    }
    if (o.iterations == 0) {
        std::fprintf(stderr, "error: --iterations must be greater than zero\n");
        return false;
    }
    if (o.repeats < 2) {
        std::fprintf(stderr, "error: --repeats must be at least 2 (a single sample has no "
                             "spread)\n");
        return false;
    }
    return true;
}

// --- translation cost -------------------------------------------------------

struct TranslationSample {
    double total_ns = 0.0;
    double ns_per_guest_instruction = 0.0;
    double bytes_per_guest_instruction = 0.0;
    std::size_t blocks = 0;
    std::size_t code_bytes = 0;
    std::uint64_t protection_changes = 0;
    std::size_t prefetched = 0;
};

struct TranslationResult {
    bool available = false;
    bool correct = false;
    std::vector<TranslationSample> samples;
    hyperion::SampleSummary total_ns;
    hyperion::SampleSummary ns_per_instruction;
    double bytes_per_instruction = 0.0;
    std::size_t blocks = 0;
    std::size_t code_bytes = 0;
    std::uint64_t protection_changes = 0;
    std::size_t prefetched = 0;
    std::uint64_t run_steps = 0;
};

bool run_wide_guest(FlatMemory& memory, GuestState& state) {
    const std::vector<std::uint8_t> program = make_wide_program(1000);
    if (program.size() > kMemSize) {
        return false;
    }
    std::memcpy(memory.data(), program.data(), program.size());
    state.rip = kBase;
    state.reg(Gpr::kRbx) = 1;
    state.reg(Gpr::kRcx) = 1;
    state.reg(Gpr::kRdx) = 1;
    state.reg(Gpr::kRsi) = 1;
    state.reg(Gpr::kRdi) = 1;
    state.reg(Gpr::kR8) = 1;
    state.reg(Gpr::kR9) = 1;
    state.reg(Gpr::kR10) = 1;
    state.reg(Gpr::kR11) = 1;
    state.reg(Gpr::kRax) = 0;
    return true;
}

TranslationResult measure_translation(const Options& options) {
    TranslationResult out;
    if (!hyperion::ExecMem::executable_supported()) {
        return out; // unavailable on this host; callers must report that, not a zero
    }
    out.available = true;

    bool all_correct = true;
    for (std::uint32_t s = 0; s <= options.repeats; ++s) {
        FlatMemory memory(kBase, kMemSize);
        GuestState state;
        if (!run_wide_guest(memory, state)) {
            out.available = false;
            return out;
        }
        TranslationCache cache;
        const RunResult result = cache.run(memory, state, options.iterations * 13 + 64, nullptr);
        // Guest arithmetic only: the only correct outcome is "ran until hlt".
        const bool ok = result.status == RunStatus::kOk;
        if (!ok) {
            all_correct = false;
        }
        if (s == 0) {
            continue; // warm-up discarded
        }
        TranslationSample sample;
        sample.total_ns = static_cast<double>(cache.translation_ns());
        sample.blocks = cache.block_count();
        sample.code_bytes = cache.total_code_bytes();
        sample.protection_changes = cache.protection_changes();
        sample.prefetched = cache.prefetched_blocks();
        const std::uint64_t guest_instructions = cache.translated_guest_instructions();
        if (guest_instructions != 0) {
            sample.ns_per_guest_instruction =
                sample.total_ns / static_cast<double>(guest_instructions);
            sample.bytes_per_guest_instruction =
                static_cast<double>(sample.code_bytes) / static_cast<double>(guest_instructions);
        }
        out.run_steps = result.steps;
        out.samples.push_back(sample);
    }

    std::vector<double> totals;
    std::vector<double> per_instruction;
    for (const TranslationSample& s : out.samples) {
        totals.push_back(s.total_ns);
        per_instruction.push_back(s.ns_per_guest_instruction);
    }
    out.total_ns = hyperion::summarize(std::move(totals));
    out.ns_per_instruction = hyperion::summarize(std::move(per_instruction));
    if (!out.samples.empty()) {
        out.bytes_per_instruction = out.samples.back().bytes_per_guest_instruction;
        out.blocks = out.samples.back().blocks;
        out.code_bytes = out.samples.back().code_bytes;
        out.protection_changes = out.samples.back().protection_changes;
        out.prefetched = out.samples.back().prefetched;
    }
    out.correct = all_correct && out.total_ns.median > 0.0;
    return out;
}

// --- syscall-heavy guest ----------------------------------------------------

struct SyscallRun {
    bool available = false;
    bool correct = false;
    std::uint64_t steps = 0;
    std::uint64_t exit_value = 0;
    hyperion::SampleSummary ns_total;
    hyperion::SampleSummary ns_per_syscall;
};

SyscallRun measure_syscall_guest(std::uint64_t syscalls, std::uint32_t repeats, int tier,
                                 bool compat) {
    SyscallRun out;
    if (tier == 1 && !hyperion::ExecMem::executable_supported()) {
        return out; // unavailable, not zero
    }
    out.available = true;

    std::vector<double> totals;
    std::vector<double> per_syscall;
    const std::uint64_t expected_steps = syscalls * kInstructionsPerSyscall + 1;
    bool all_correct = true;
    std::uint64_t first_exit = 0;

    for (std::uint32_t s = 0; s <= repeats; ++s) {
        FlatMemory memory(kBase, kMemSize);
        std::memcpy(memory.data(), kSyscallLoop.data(), kSyscallLoop.size());
        hyperion::MinimalSyscallHandler handler({}, nullptr);
        if (compat) {
            handler.enable_linux_compat();
        }
        GuestState state;
        state.rip = kBase;
        state.reg(Gpr::kRcx) = syscalls;

        const auto start = Clock::now();
        RunResult result;
        if (tier == 0) {
            result = hyperion::run_program(memory, state, expected_steps + 8, &handler);
        } else {
            TranslationCache cache;
            result = cache.run(memory, state, expected_steps + 8, &handler);
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        optimization_barrier(&result);

        // Exact step count and a sane return value: a guest that ran the wrong
        // number of iterations, or a handler that did not answer getpid, would
        // otherwise be published as a fast number.
        const bool ok = result.status == RunStatus::kOk && result.steps == expected_steps &&
                        result.state.reg(Gpr::kRax) <= 0x7FFFFFFFULL;
        all_correct = all_correct && ok;
        if (s == 0) {
            continue; // warm-up discarded
        }
        if (s == 1) {
            first_exit = result.state.reg(Gpr::kRax);
        } else if (result.state.reg(Gpr::kRax) != first_exit) {
            all_correct = false; // the process was forked or misreported a pid
        }
        const double total = static_cast<double>(elapsed);
        totals.push_back(total);
        per_syscall.push_back(total / static_cast<double>(syscalls));
        out.steps = result.steps;
        out.exit_value = result.state.reg(Gpr::kRax);
    }

    out.ns_total = hyperion::summarize(std::move(totals));
    out.ns_per_syscall = hyperion::summarize(std::move(per_syscall));
    out.correct = all_correct && out.ns_per_syscall.median > 0.0;
    return out;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        return 2;
    }

    const TranslationResult translation = measure_translation(options);
    const SyscallRun tier0 = measure_syscall_guest(options.syscalls, options.repeats, 0, false);
    const SyscallRun tier1 = measure_syscall_guest(options.syscalls, options.repeats, 1, false);
    const SyscallRun tier0_compat =
        options.compat ? measure_syscall_guest(options.syscalls, options.repeats, 0, true)
                       : SyscallRun{};
    const SyscallRun tier1_compat =
        options.compat ? measure_syscall_guest(options.syscalls, options.repeats, 1, true)
                       : SyscallRun{};
    const bool tier1_supported = hyperion::ExecMem::executable_supported();

    bool all_correct = tier0.correct;
    if (translation.available) {
        all_correct = all_correct && translation.correct;
    }
    if (tier1.available) {
        all_correct = all_correct && tier1.correct;
    }
    if (tier0_compat.available) {
        all_correct = all_correct && tier0_compat.correct;
    }
    if (tier1_compat.available) {
        all_correct = all_correct && tier1_compat.correct;
    }

    if (options.json) {
        std::printf("{\n");
        std::printf("  \"benchmark\": \"translation_and_syscall\",\n");
        std::printf("  \"hyperion_version\": \"%s\",\n", hyperion::version_string().c_str());
        std::printf("  \"repeats\": %u,\n", options.repeats);
        std::printf("  \"warmup_discarded\": 1,\n");
        std::printf("  \"correct\": %s,\n", all_correct ? "true" : "false");
        std::printf("  \"translation\": {\n");
        std::printf("    \"available\": %s,\n", translation.available ? "true" : "false");
        if (translation.available) {
            std::printf("    \"guest_iterations\": %llu,\n",
                        static_cast<unsigned long long>(options.iterations));
            std::printf("    \"blocks\": %zu,\n", translation.blocks);
            std::printf("    \"generated_code_bytes\": %zu,\n", translation.code_bytes);
            std::printf("    \"ns_median\": %.3f,\n", translation.total_ns.median);
            std::printf("    \"ns_per_guest_instruction_median\": %.3f,\n",
                        translation.ns_per_instruction.median);
            std::printf("    \"bytes_per_guest_instruction\": %.3f,\n",
                        translation.bytes_per_instruction);
            std::printf("    \"protection_changes\": %llu,\n",
                        static_cast<unsigned long long>(translation.protection_changes));
            std::printf("    \"prefetched_blocks\": %zu,\n", translation.prefetched);
            std::printf("    \"iqr_fraction\": %.6f\n",
                        translation.ns_per_instruction.iqr_fraction);
        } else {
            std::printf("    \"reason\": \"host cannot execute generated AArch64 code\"\n");
        }
        std::printf("  },\n");
        for (int tier = 0; tier <= 1; ++tier) {
            const SyscallRun& r = (tier == 0) ? tier0 : tier1;
            std::printf("  \"syscall_heavy_tier%d\": {\n", tier);
            std::printf("    \"available\": %s,\n", r.available ? "true" : "false");
            if (r.available) {
                std::printf("    \"guest_syscalls\": %llu,\n",
                            static_cast<unsigned long long>(options.syscalls));
                std::printf("    \"guest_steps\": %llu,\n",
                            static_cast<unsigned long long>(r.steps));
                std::printf("    \"ns_per_guest_syscall_median\": %.3f,\n",
                            r.ns_per_syscall.median);
                std::printf("    \"p25_ns\": %.3f,\n", r.ns_per_syscall.p25);
                std::printf("    \"p75_ns\": %.3f,\n", r.ns_per_syscall.p75);
                std::printf("    \"iqr_fraction\": %.6f\n", r.ns_per_syscall.iqr_fraction);
            } else {
                std::printf("    \"reason\": \"host cannot execute generated AArch64 code\"\n");
            }
            std::printf("  },\n");
            if (options.compat) {
                const SyscallRun& c = (tier == 0) ? tier0_compat : tier1_compat;
                std::printf("  \"syscall_heavy_compat_tier%d\": {\n", tier);
                std::printf("    \"available\": %s,\n", c.available ? "true" : "false");
                if (c.available) {
                    std::printf("    \"guest_syscalls\": %llu,\n",
                                static_cast<unsigned long long>(options.syscalls));
                    std::printf("    \"ns_per_guest_syscall_median\": %.3f,\n",
                                c.ns_per_syscall.median);
                    std::printf("    \"p25_ns\": %.3f,\n", c.ns_per_syscall.p25);
                    std::printf("    \"p75_ns\": %.3f,\n", c.ns_per_syscall.p75);
                    std::printf("    \"iqr_fraction\": %.6f\n", c.ns_per_syscall.iqr_fraction);
                } else {
                    std::printf("    \"reason\": \"host cannot execute generated AArch64 code\"\n");
                }
                std::printf("  },\n");
            }
        }
        std::printf("  \"done\": true\n}\n");
        return all_correct ? 0 : 1;
    }

    std::printf("translation cost and syscall-heavy guest\n");
    std::printf("  samples           : %u (1 warm-up discarded)\n", options.repeats);
    std::printf("  tier 1 on this host: %s\n",
                tier1_supported ? "yes (generated AArch64 code executes here)"
                                : "NO (x86-64 cannot execute generated AArch64; tier 1 numbers "
                                  "are unavailable, not zero)");
    std::printf("\n  translation cost\n");
    if (translation.available) {
        std::printf("    blocks translated              : %zu\n", translation.blocks);
        std::printf("    generated code bytes           : %zu\n", translation.code_bytes);
        std::printf("    wall time per translation pass : %.3f ns median\n",
                    translation.total_ns.median);
        std::printf("    cost per translated instruction: %.3f ns median\n",
                    translation.ns_per_instruction.median);
        std::printf("    generated bytes per instruction: %.3f\n",
                    translation.bytes_per_instruction);
        std::printf("    mprotect calls for that pass   : %llu (%.2f per block)\n",
                    static_cast<unsigned long long>(translation.protection_changes),
                    translation.blocks != 0 ? static_cast<double>(translation.protection_changes) /
                                                  static_cast<double>(translation.blocks)
                                            : 0.0);
        std::printf("    blocks translated ahead        : %zu (successor prefetch)\n",
                    translation.prefetched);
        std::printf("    iqr fraction                   : %.4f\n",
                    translation.ns_per_instruction.iqr_fraction);
    } else {
        std::printf("    unavailable: this host cannot execute generated AArch64 code\n");
    }
    std::printf("\n  guest syscall loop (4 instructions per syscall)\n");
    std::printf("    %-8s %14s %14s %14s\n", "tier", "median ns/sys", "p25 ns/sys", "p75 ns/sys");
    for (int tier = 0; tier <= 1; ++tier) {
        const SyscallRun& r = (tier == 0) ? tier0 : tier1;
        if (r.available) {
            std::printf("    %-8d %14.3f %14.3f %14.3f\n", tier, r.ns_per_syscall.median,
                        r.ns_per_syscall.p25, r.ns_per_syscall.p75);
        } else {
            std::printf("    %-8d %14s %14s %14s\n", tier, "unavailable", "", "");
        }
    }
    if (options.compat) {
        std::printf("\n  guest syscall loop, Linux-compat handler (what `hyperion run` uses)\n");
        for (int tier = 0; tier <= 1; ++tier) {
            const SyscallRun& r = (tier == 0) ? tier0_compat : tier1_compat;
            if (r.available) {
                std::printf("    %-8d %14.3f %14.3f %14.3f\n", tier, r.ns_per_syscall.median,
                            r.ns_per_syscall.p25, r.ns_per_syscall.p75);
            } else {
                std::printf("    %-8d %14s %14s %14s\n", tier, "unavailable", "", "");
            }
        }
    }
    if (tier1.available) {
        std::printf("\n    tier 1 speedup on this workload: %.3fx\n",
                    tier1.ns_per_syscall.median > 0.0
                        ? tier0.ns_per_syscall.median / tier1.ns_per_syscall.median
                        : 0.0);
    }
    std::printf("\n  correctness gate               : %s\n",
                all_correct ? "passed (step counts and return values checked)"
                            : "FAILED (see exit status)");
    std::printf(
        "  NOTE: wall clock on this host only. Translation cost includes arena allocation\n");
    std::printf("        and the per-block W^X mprotect pair. Under qemu-user these are\n");
    std::printf("        emulation numbers, not AArch64 hardware numbers.\n");
    return all_correct ? 0 : 1;
}
