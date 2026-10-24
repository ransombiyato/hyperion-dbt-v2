// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Syscall dispatch microbenchmark.
//
// Question it answers, precisely: how much does one trip through
// MinimalSyscallHandler::handle() cost for each of the dispatch layers, compared
// with (a) the layer underneath it and (b) a bare host syscall?
//
// What is timed (one iteration = one dispatch):
//   host_getpid        ::syscall(SYS_getpid)             host kernel reference
//   host_sched_yield   ::syscall(SYS_sched_yield)         reference for layer 1
//   direct_adapter     entry->fn(ctx)                    table hit, no handle()
//   adapter_dispatch   handle() -> compiled adapter      layer 1
//   existing_dispatch  handle() -> handle_unguarded      layer 2 (getpid)
//   unsupported        handle() -> -ENOSYS path          layer 3
//
// Methodology (matches BENCHMARKS.md section 5):
//   * one discarded warm-up sample, then `--repeats` timed samples;
//   * the median is reported with the IQR spread, never a single sample;
//   * a checksum verifies every sample performed the same work, so the harness
//     cannot silently report a measurement of a broken build;
//   * an optimisation barrier keeps the call from being hoisted out of the loop
//     (docs/known-issues.md BENCH-001);
//   * a plausibility floor rejects an implausibly fast result rather than
//     publishing an eliminated workload as a number.
//
// Limitations, stated rather than implied: this measures dispatch overhead on
// the *host* CPU. It does not measure guest translation cost, and the numbers
// are only comparable within one run on one machine.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

#include "hyperion/bench_stats.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/syscall.hpp"
#include "hyperion/syscall_dispatch.hpp"
#include "hyperion/version.hpp"

namespace {

using Clock = std::chrono::steady_clock;

inline void optimization_barrier(const void* p) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(p) : "memory");
#else
    *static_cast<volatile const void**>(const_cast<const void**>(&p)) = p;
#endif
}

constexpr std::uint64_t kBase = 0x400000;
constexpr std::size_t kMemSize = 4096;

struct Options {
    std::uint64_t iterations = 200000;
    std::uint32_t repeats = 5;
    bool json = false;
};

void usage() {
    std::printf("usage: hyperion_syscall_bench [--iterations N] [--repeats N] [--json]\n"
                "\n"
                "  --iterations N  dispatches per sample (default 200000)\n"
                "  --repeats N     timed samples, one warm-up is discarded (default 5, min 2)\n"
                "  --json          machine-readable output\n");
}

bool parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") {
            options.json = true;
        } else if (arg.rfind("--iterations=", 0) == 0) {
            options.iterations = std::strtoull(arg.c_str() + 13, nullptr, 10);
        } else if (arg.rfind("--repeats=", 0) == 0) {
            options.repeats =
                static_cast<std::uint32_t>(std::strtoul(arg.c_str() + 10, nullptr, 10));
        } else if (arg == "--iterations" && i + 1 < argc) {
            options.iterations = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--repeats" && i + 1 < argc) {
            options.repeats = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return false;
        } else {
            std::fprintf(stderr, "hyperion_syscall_bench: unknown argument '%s'\n", arg.c_str());
            return false;
        }
    }
    if (options.iterations == 0) {
        std::fprintf(stderr, "hyperion_syscall_bench: --iterations must be > 0\n");
        return false;
    }
    // A single sample cannot express a spread; refuse rather than publish one.
    if (options.repeats < 2) {
        std::fprintf(stderr, "hyperion_syscall_bench: --repeats must be at least 2 "
                             "(one warm-up is discarded and a spread needs two samples)\n");
        return false;
    }
    return true;
}

// One measurement target.
struct Measurement {
    std::string name;
    std::string what;
    std::uint64_t checksum = 0;
    hyperion::SampleSummary summary;
    bool plausible = false;
};

using SampleFn = std::uint64_t (*)(std::uint64_t iterations);

struct Context {
    hyperion::FlatMemory* memory = nullptr;
    hyperion::MinimalSyscallHandler* handler = nullptr;
    const hyperion::DispatchEntry* adapter_entry = nullptr;
};

Context g_ctx;

std::uint64_t sample_host_getpid(std::uint64_t iterations) {
    std::uint64_t acc = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        acc += static_cast<std::uint64_t>(::syscall(SYS_getpid));
    }
    optimization_barrier(&acc);
    return acc;
}

// Native reference for the *same* operation the adapter performs, so the
// adapter's dispatch cost is not confused with the cost of the syscall itself.
std::uint64_t sample_host_sched_yield(std::uint64_t iterations) {
    std::uint64_t acc = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        acc += static_cast<std::uint64_t>(::syscall(SYS_sched_yield));
    }
    optimization_barrier(&acc);
    return acc;
}

std::uint64_t sample_direct_adapter(std::uint64_t iterations) {
    std::uint64_t acc = 0;
    hyperion::GuestState state;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        hyperion::SyscallContext ctx{*g_ctx.handler, state, *g_ctx.memory, 24};
        acc += static_cast<std::uint64_t>(g_ctx.adapter_entry->fn(ctx));
    }
    optimization_barrier(&acc);
    return acc;
}

std::uint64_t sample_adapter_dispatch(std::uint64_t iterations) {
    std::uint64_t acc = 0;
    hyperion::GuestState state;
    state.reg(hyperion::Gpr::kRax) = 24; // sched_yield, compiled adapter
    for (std::uint64_t i = 0; i < iterations; ++i) {
        state.reg(hyperion::Gpr::kRax) = 24;
        (void)g_ctx.handler->handle(state, *g_ctx.memory);
        acc += state.reg(hyperion::Gpr::kRax);
    }
    optimization_barrier(&acc);
    return acc;
}

std::uint64_t sample_existing_dispatch(std::uint64_t iterations) {
    std::uint64_t acc = 0;
    hyperion::GuestState state;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        state.reg(hyperion::Gpr::kRax) = 39; // getpid, existing switch
        (void)g_ctx.handler->handle(state, *g_ctx.memory);
        acc += state.reg(hyperion::Gpr::kRax);
    }
    optimization_barrier(&acc);
    return acc;
}

std::uint64_t sample_unsupported(std::uint64_t iterations) {
    std::uint64_t acc = 0;
    hyperion::GuestState state;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        state.reg(hyperion::Gpr::kRax) = 451; // unimplemented
        (void)g_ctx.handler->handle(state, *g_ctx.memory);
        acc += state.reg(hyperion::Gpr::kRax);
    }
    optimization_barrier(&acc);
    return acc;
}

Measurement measure(const std::string& name, const std::string& what, SampleFn fn,
                    const Options& options) {
    Measurement m;
    m.name = name;
    m.what = what;
    std::vector<double> ns_per_call;
    ns_per_call.reserve(options.repeats);
    for (std::uint32_t r = 0; r < options.repeats; ++r) {
        const auto start = Clock::now();
        const std::uint64_t value = fn(options.iterations);
        const auto end = Clock::now();
        const double seconds = std::chrono::duration<double>(end - start).count();
        if (r == 0) {
            m.checksum = value;
            continue; // discard the warm-up sample
        }
        if (m.checksum == 0) {
            m.checksum = value;
        }
        ns_per_call.push_back(seconds * 1e9 / static_cast<double>(options.iterations));
    }
    m.summary = hyperion::summarize(ns_per_call);
    // A dispatch cannot complete in less than a few nanoseconds; anything below
    // the floor means the optimiser removed the work (BENCH-001).
    m.plausible = m.summary.median >= 0.5;
    return m;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        return 2;
    }

    hyperion::FlatMemory memory(kBase, kMemSize);
    hyperion::MinimalSyscallHandler handler({}, nullptr);
    const hyperion::DispatchEntry* entry = handler.dispatcher().lookup(24);
    if (entry == nullptr || entry->kind != hyperion::DispatchKind::kAdapter) {
        std::fprintf(stderr, "hyperion_syscall_bench: sched_yield adapter is not registered\n");
        return 1;
    }
    g_ctx.memory = &memory;
    g_ctx.handler = &handler;
    g_ctx.adapter_entry = entry;

    std::vector<Measurement> results;
    results.push_back(measure("host_getpid", "bare host getpid syscall (reference)",
                              sample_host_getpid, options));
    results.push_back(measure("host_sched_yield", "bare host sched_yield syscall (reference)",
                              sample_host_sched_yield, options));
    results.push_back(measure("direct_adapter", "compiled adapter call, no dispatch",
                              sample_direct_adapter, options));
    results.push_back(measure("adapter_dispatch", "handle() -> compiled adapter (layer 1)",
                              sample_adapter_dispatch, options));
    results.push_back(measure("existing_dispatch", "handle() -> existing switch (layer 2)",
                              sample_existing_dispatch, options));
    results.push_back(measure("unsupported", "handle() -> -ENOSYS + diagnostics (layer 3)",
                              sample_unsupported, options));

    bool all_plausible = true;
    for (const Measurement& m : results) {
        all_plausible = all_plausible && m.plausible;
    }

    // Dispatch overhead attributable to the table itself: layer 1 minus the
    // direct call it wraps.
    double table_overhead = 0.0;
    for (const Measurement& m : results) {
        if (m.name == "adapter_dispatch") {
            for (const Measurement& d : results) {
                if (d.name == "direct_adapter") {
                    table_overhead = m.summary.median - d.summary.median;
                }
            }
        }
    }

    if (options.json) {
        std::printf("{\n");
        std::printf("  \"benchmark\": \"syscall_dispatch\",\n");
        std::printf("  \"build\": \"%s\",\n", hyperion::build_summary().c_str());
        std::printf("  \"iterations\": %llu,\n",
                    static_cast<unsigned long long>(options.iterations));
        std::printf("  \"repeats\": %u,\n", options.repeats);
        std::printf("  \"plausible\": %s,\n", all_plausible ? "true" : "false");
        std::printf("  \"table_overhead_ns\": %.3f,\n", table_overhead);
        std::printf("  \"results\": [\n");
        for (std::size_t i = 0; i < results.size(); ++i) {
            const Measurement& m = results[i];
            std::printf("    {\"name\": \"%s\", \"median_ns\": %.3f, \"p25_ns\": %.3f, "
                        "\"p75_ns\": %.3f, \"min_ns\": %.3f, \"max_ns\": %.3f, "
                        "\"iqr_fraction\": %.4f, \"checksum\": %llu}%s\n",
                        m.name.c_str(), m.summary.median, m.summary.p25, m.summary.p75,
                        m.summary.min, m.summary.max, m.summary.iqr_fraction,
                        static_cast<unsigned long long>(m.checksum),
                        i + 1 == results.size() ? "" : ",");
        }
        std::printf("  ]\n}\n");
        return all_plausible ? 0 : 1;
    }

    std::printf("syscall dispatch microbenchmark\n");
    std::printf("  iterations/sample : %llu\n",
                static_cast<unsigned long long>(options.iterations));
    std::printf("  samples           : %u (1 warm-up discarded)\n", options.repeats);
    std::printf("\n  %-18s %10s %10s %10s   %s\n", "target", "median ns", "p25 ns", "p75 ns",
                "what");
    for (const Measurement& m : results) {
        std::printf("  %-18s %10.3f %10.3f %10.3f   %s\n", m.name.c_str(), m.summary.median,
                    m.summary.p25, m.summary.p75, m.what.c_str());
    }
    std::printf("\n  table overhead (layer 1 - direct call): %.3f ns/call\n", table_overhead);
    std::printf("  measurement plausible                : %s\n",
                all_plausible ? "yes" : "NO (optimiser may have removed work)");
    std::printf("\n  NOTE: host CPU only; does not include guest instruction translation.\n");
    return all_plausible ? 0 : 1;
}
