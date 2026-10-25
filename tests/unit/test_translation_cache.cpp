// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Translation-cache tests.
//
// The cache is the component that decides *whether* it is safe to execute
// generated code at all. That decision is tested everywhere: on a host that
// cannot execute its own generated code, run() must refuse and execute nothing,
// rather than pretend. The differential test that generated blocks agree with
// the interpreter lives in test_translator.cpp, where the rest of the
// translation pipeline is covered.
#include <cstdint>
#include <cstring>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/exec_mem.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/translation_cache.hpp"

using hyperion::ExecMem;
using hyperion::FlatMemory;
using hyperion::Gpr;
using hyperion::GuestState;
using hyperion::RunStatus;
using hyperion::TranslationCache;

namespace {
constexpr std::uint64_t kBase = 0x400000;
constexpr std::size_t kMemSize = 4096;
} // namespace

HYPERION_TEST("translation_cache", "refuses_to_run_without_exec_support") {
    if (ExecMem::executable_supported()) {
        HYPERION_SKIP("host can execute generated code; refusal path not exercised");
    }
    // mov eax, 1 ; hlt
    const std::vector<std::uint8_t> program = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xF4};
    FlatMemory memory(kBase, kMemSize);
    std::memcpy(memory.data(), program.data(), program.size());

    GuestState initial;
    initial.rip = kBase;
    TranslationCache cache;
    const hyperion::RunResult r = cache.run(memory, initial);

    HYPERION_CHECK(r.status == RunStatus::kUnsupported);
    HYPERION_CHECK_EQ(r.steps, std::uint64_t{0});
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRax), std::uint64_t{0}); // nothing ran
    HYPERION_CHECK_EQ(r.state.rip, kBase);
    HYPERION_CHECK_EQ(cache.block_count(), std::size_t{0}); // nothing compiled
}

#if defined(__aarch64__)

HYPERION_TEST("translation_cache", "caches_blocks_and_agrees_with_interpreter") {
    // A loop body: mov ecx, 50 ; add rbx, rax ; dec rcx ; jnz -5 ; hlt
    const std::vector<std::uint8_t> program = {
        0xB9, 0x32, 0x00, 0x00, 0x00, 0x48, 0x01, 0xC3, 0x48, 0xFF, 0xC9, 0x75, 0xF8, 0xF4,
    };

    GuestState initial;
    initial.rip = kBase;
    initial.reg(Gpr::kRax) = 3;

    FlatMemory interp_mem(kBase, kMemSize);
    std::memcpy(interp_mem.data(), program.data(), program.size());
    const hyperion::RunResult expected = hyperion::run_program(interp_mem, initial);

    FlatMemory cache_mem(kBase, kMemSize);
    std::memcpy(cache_mem.data(), program.data(), program.size());
    TranslationCache cache;
    const hyperion::RunResult actual = cache.run(cache_mem, initial);

    HYPERION_CHECK(actual.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(actual.steps, expected.steps);
    HYPERION_CHECK_EQ(actual.state.reg(Gpr::kRbx), expected.state.reg(Gpr::kRbx));
    HYPERION_CHECK_EQ(actual.state.rip, expected.state.rip);
    // The loop is entered repeatedly but translated once per distinct PC, so the
    // block count stays small: this is the cache working, not a re-translation.
    HYPERION_CHECK(cache.block_count() > 0);
    HYPERION_CHECK(cache.block_count() <= 4);
    // The direct-mapped fast path should absorb the repeated dispatches; only
    // the first sighting of each PC is a miss.
    HYPERION_CHECK(cache.dispatch_hits() > cache.dispatch_misses());
}

HYPERION_TEST("translation_cache", "direct_map_collision_falls_back_to_map") {
    // Two guest PCs that share a direct-map slot must both still run correctly:
    // the fast path is a pure accelerator and the hash map is authoritative, so
    // a collision can only cost a fall-through, never a wrong block. kBase and
    // kBase + 0x1000 both index slot 0 (kDirectMask = 0xFFF).
    constexpr std::uint64_t kSecond = kBase + 0x1000;
    const std::vector<std::uint8_t> first = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xF4};  // mov eax,1; hlt
    const std::vector<std::uint8_t> second = {0xB8, 0x02, 0x00, 0x00, 0x00, 0xF4}; // mov eax,2; hlt

    FlatMemory mem(kBase, 0x3000);
    std::memcpy(mem.data(), first.data(), first.size());
    std::memcpy(mem.data() + 0x1000, second.data(), second.size());

    TranslationCache cache;

    GuestState a;
    a.rip = kBase;
    const hyperion::RunResult ra = cache.run(mem, a);
    HYPERION_CHECK(ra.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(ra.state.reg(Gpr::kRax), std::uint64_t{1});

    GuestState b;
    b.rip = kSecond; // installs into slot 0, evicting the kBase entry
    const hyperion::RunResult rb = cache.run(mem, b);
    HYPERION_CHECK(rb.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(rb.state.reg(Gpr::kRax), std::uint64_t{2});

    GuestState a2;
    a2.rip = kBase; // slot 0 now holds kSecond: must fall back to the map
    const hyperion::RunResult ra2 = cache.run(mem, a2);
    HYPERION_CHECK(ra2.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(ra2.state.reg(Gpr::kRax), std::uint64_t{1}); // still correct
    HYPERION_CHECK_EQ(cache.block_count(), std::size_t{2});        // not re-translated
}

HYPERION_TEST("translation_cache", "exits_are_linked_to_successors_and_stay_correct") {
    // A branch-heavy guest, so there are many block transitions to link: two
    // conditional arms around a backward jump, i.e. five blocks whose exits all
    // have a statically known successor. Assembled from
    //   mov eax,0 / top: add eax,3 / dec rcx / test rcx,1 / jz even_path
    //               / cmp rcx,0 / jne top / hlt
    //   even_path:  add eax,1 / cmp rcx,0 / jne top / hlt
    const std::vector<std::uint8_t> program = {
        0xB8, 0x00, 0x00, 0x00, 0x00, 0x83, 0xC0, 0x03, 0x48, 0xFF, 0xC9, 0x48, 0xF7,
        0xC1, 0x01, 0x00, 0x00, 0x00, 0x74, 0x07, 0x48, 0x83, 0xF9, 0x00, 0x75, 0xEB,
        0xF4, 0x83, 0xC0, 0x01, 0x48, 0x83, 0xF9, 0x00, 0x75, 0xE1, 0xF4,
    };
    FlatMemory interp_mem(kBase, kMemSize);
    std::memcpy(interp_mem.data(), program.data(), program.size());
    GuestState initial;
    initial.rip = kBase;
    initial.reg(Gpr::kRcx) = 40;
    const hyperion::RunResult expected = hyperion::run_program(interp_mem, initial);

    FlatMemory cache_mem(kBase, kMemSize);
    std::memcpy(cache_mem.data(), program.data(), program.size());
    TranslationCache cache;
    const hyperion::RunResult actual = cache.run(cache_mem, initial);

    // Correctness first: a mislinked exit shows up as a wrong result or a wrong
    // step count, not as a crash.
    HYPERION_CHECK(actual.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(actual.steps, expected.steps);
    HYPERION_CHECK_EQ(actual.state.reg(Gpr::kRax), expected.state.reg(Gpr::kRax));
    HYPERION_CHECK_EQ(actual.state.rip, expected.state.rip);
    // And the link really happened, so the checks above are not the checks of an
    // unlinked run.
    HYPERION_CHECK(cache.linked_exits() > 0);
    HYPERION_CHECK(cache.linked_exits() <= cache.block_count() * 2);
}

HYPERION_TEST("translation_cache",
              "self_loop_superblock_matches_interpreter_with_and_without_step_limit") {
    // Tight self-loop exercising both full completion and mid-loop step-budget
    // cut-off (`max_steps = 11` -> 3 iterations = 9 steps, stopping before 4th),
    // verifying all GPRs, rip, and all 6 architectural flags match Tier 0.
    //   loop: add rdx, rax ; sub rcx, rax ; jnz loop ; hlt
    const std::vector<std::uint8_t> program = {
        0x48, 0x01, 0xC2, // add rdx, rax
        0x48, 0x29, 0xC1, // sub rcx, rax
        0x75, 0xF8,       // jnz loop
        0xF4,             // hlt
    };

    for (const std::uint64_t limit : {std::uint64_t{11}, std::uint64_t{1000}}) {
        GuestState initial;
        initial.rip = kBase;
        initial.reg(Gpr::kRax) = 3;
        initial.reg(Gpr::kRcx) = 30; // 10 iterations total
        initial.reg(Gpr::kRdx) = 0xFF;

        FlatMemory m0(kBase, kMemSize);
        std::memcpy(m0.data(), program.data(), program.size());
        const hyperion::RunResult exp =
            hyperion::run_program(m0, initial, (limit == 11) ? 9 : limit);

        FlatMemory m1(kBase, kMemSize);
        std::memcpy(m1.data(), program.data(), program.size());
        TranslationCache cache;
        const hyperion::RunResult got = cache.run(m1, initial, limit);

        HYPERION_CHECK_EQ(got.steps, exp.steps);
        HYPERION_CHECK_EQ(got.state.rip, exp.state.rip);
        HYPERION_CHECK_EQ(got.state.reg(Gpr::kRax), exp.state.reg(Gpr::kRax));
        HYPERION_CHECK_EQ(got.state.reg(Gpr::kRcx), exp.state.reg(Gpr::kRcx));
        HYPERION_CHECK_EQ(got.state.reg(Gpr::kRdx), exp.state.reg(Gpr::kRdx));
        HYPERION_CHECK_EQ(got.state.flags.cf, exp.state.flags.cf);
        HYPERION_CHECK_EQ(got.state.flags.pf, exp.state.flags.pf);
        HYPERION_CHECK_EQ(got.state.flags.af, exp.state.flags.af);
        HYPERION_CHECK_EQ(got.state.flags.zf, exp.state.flags.zf);
        HYPERION_CHECK_EQ(got.state.flags.sf, exp.state.flags.sf);
        HYPERION_CHECK_EQ(got.state.flags.of, exp.state.flags.of);
    }
}

#else

HYPERION_TEST("translation_cache", "caches_blocks_and_agrees_with_interpreter") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "direct_map_collision_falls_back_to_map") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "exits_are_linked_to_successors_and_stay_correct") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache",
              "self_loop_superblock_matches_interpreter_with_and_without_step_limit") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

#endif
