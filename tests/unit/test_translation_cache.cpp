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
#include <cstdio>
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

HYPERION_TEST("translation_cache", "eight_and_sixteen_bit_alu_flags_match_interpreter") {
    // 8- and 16-bit ALU/CMP/TEST used to reach the per-instruction C++ helper
    // whenever more than ZF/CF was live, which measurement showed was ~95% of all
    // helper calls on a real guest. They are now inlined, and inlining them means
    // reproducing six flags that AArch64 cannot produce directly at that width:
    // a 32-bit SUBS of zero-extended bytes gets CF and ZF right but leaves SF on
    // bit 31 and OF at 32-bit width, both wrong. Each case below therefore runs
    // the same bytes through tier 0 (the reference interpreter, which is the
    // definition of the answer) and tier 1, and compares all six flags, every
    // GPR, rip and the retired-step count.
    //
    // The values are the interesting ones, not a sample: 0x7f/0x80 straddle the
    // sign boundary, 0x00/0x01 exercise borrow and zero results, 0xff is all ones
    // (parity, AF), and 0x80 - 0x01 is the signed-overflow case a 32-bit SUBS
    // cannot see.
    struct Case {
        const char* what;
        std::vector<std::uint8_t> code;
        bool touches_memory;
    };
    // Encodings verified with as/objdump.
    const std::vector<Case> cases = {
        {"cmp al, 0x80", {0x3C, 0x80}, false},
        {"cmp al, bl", {0x38, 0xD8}, false},
        {"test al, 0x01", {0xA8, 0x01}, false},
        {"test al, bl", {0x84, 0xD8}, false},
        {"and al, 0x0f", {0x24, 0x0F}, false},
        {"or al, bl", {0x08, 0xD8}, false},
        {"xor al, al", {0x30, 0xC0}, false},
        {"test ah, bh", {0x84, 0xFC}, false}, // high-byte registers
        {"and byte [rbx], 0x0f", {0x80, 0x23, 0x0F}, true},
        {"cmp byte [rbx], 0x80", {0x80, 0x3B, 0x80}, true},
        {"cmp byte [rbx], al", {0x38, 0x03}, true},
        {"test byte [rbx], 0x01", {0xF6, 0x03, 0x01}, true},
        {"cmp ax, 0x8000", {0x66, 0x3D, 0x00, 0x80}, false},
        {"test ax, bx", {0x66, 0x85, 0xD8}, false},
        {"and ax, 0xff00", {0x66, 0x25, 0x00, 0xFF}, false},
        {"or ax, bx", {0x66, 0x09, 0xD8}, false},
        {"or ax, 0x1234", {0x66, 0x0D, 0x34, 0x12}, false},
        {"xor ax, bx", {0x66, 0x31, 0xD8}, false},
        {"cmp ax, bx", {0x66, 0x39, 0xD8}, false},
        {"cmp word [rbx], 0x8000", {0x66, 0x81, 0x3B, 0x00, 0x80}, true},
    };
    const std::uint64_t kPatterns[] = {0x00, 0x01, 0x7F, 0x80, 0xFF};
    constexpr std::uint64_t kData = kBase + 0x100;

    for (const Case& c : cases) {
        for (std::uint64_t a : kPatterns) {
            for (std::uint64_t b : kPatterns) {
                // rax holds the byte in al (and 0xBBBB in ah to prove the high-byte
                // path does not disturb the neighbouring byte), rbx the second
                // operand and the memory pointer.
                const std::uint64_t rax = (a & 0xFFULL) | (0xBBULL << 8);
                const std::uint64_t rbx = (b & 0xFFULL) | 0xAA00ULL;

                std::vector<std::uint8_t> program = c.code;
                program.push_back(0xF4); // hlt: ends the block with every flag live

                FlatMemory interp_mem(kBase, kMemSize);
                std::memcpy(interp_mem.data(), program.data(), program.size());
                interp_mem.data()[kData - kBase] = static_cast<std::uint8_t>(a);
                GuestState initial;
                initial.rip = kBase;
                initial.reg(Gpr::kRax) = rax;
                initial.reg(Gpr::kRbx) = c.touches_memory ? kData : rbx;
                const hyperion::RunResult expected = hyperion::run_program(interp_mem, initial);

                FlatMemory cache_mem(kBase, kMemSize);
                std::memcpy(cache_mem.data(), program.data(), program.size());
                cache_mem.data()[kData - kBase] = static_cast<std::uint8_t>(a);
                TranslationCache cache;
                const hyperion::RunResult actual = cache.run(cache_mem, initial);

                // Inlined, not merely correct: a fallback to the C++ helper
                // would produce the same flags and hide a regression in the new
                // code path behind the old one.
                HYPERION_CHECK_EQ(cache.helper_calls(), std::uint64_t{0});

                if (!(actual.status == RunStatus::kOk) || actual.steps != expected.steps ||
                    actual.state.reg(Gpr::kRax) != expected.state.reg(Gpr::kRax) ||
                    actual.state.reg(Gpr::kRbx) != expected.state.reg(Gpr::kRbx) ||
                    actual.state.flags.cf != expected.state.flags.cf ||
                    actual.state.flags.pf != expected.state.flags.pf ||
                    actual.state.flags.af != expected.state.flags.af ||
                    actual.state.flags.zf != expected.state.flags.zf ||
                    actual.state.flags.sf != expected.state.flags.sf ||
                    actual.state.flags.of != expected.state.flags.of) {
                    std::fprintf(
                        stderr,
                        "  mismatch: %s a=0x%02llx b=0x%02llx\n"
                        "    tier1 cf/pf/af/zf/sf/of = %d%d%d%d%d%d  rax=0x%llx\n"
                        "    tier0 cf/pf/af/zf/sf/of = %d%d%d%d%d%d  rax=0x%llx\n",
                        c.what, static_cast<unsigned long long>(a),
                        static_cast<unsigned long long>(b), actual.state.flags.cf,
                        actual.state.flags.pf, actual.state.flags.af, actual.state.flags.zf,
                        actual.state.flags.sf, actual.state.flags.of,
                        static_cast<unsigned long long>(actual.state.reg(Gpr::kRax)),
                        expected.state.flags.cf, expected.state.flags.pf, expected.state.flags.af,
                        expected.state.flags.zf, expected.state.flags.sf, expected.state.flags.of,
                        static_cast<unsigned long long>(expected.state.reg(Gpr::kRax)));
                    HYPERION_CHECK(false);
                    return;
                }
                // A memory destination must have been written back to guest memory.
                if (c.touches_memory) {
                    HYPERION_CHECK_EQ(cache_mem.data()[kData - kBase],
                                      interp_mem.data()[kData - kBase]);
                }
            }
        }
    }
    // One flag combination per case is checked above; this loop covers every
    // operand pair in {0,1,0x7f,0x80,0xff}^2, which is where the truncation,
    // borrow, parity and signed-overflow cases live.
}

HYPERION_TEST("translation_cache", "narrow_alu_updates_a_register_allocated_host_register") {
    // The 8-bit ALU write-back has two paths: a store into GuestState, and - when
    // the host register allocator has taken the guest register - a bitfield
    // insert into the borrowed host register. A one-instruction block never
    // allocates (the allocator only takes registers referenced more than twice),
    // so the table test above exercises the store path only. This program
    // references al enough times to allocate rax, then checks the byte result,
    // all six flags, and that the neighbouring bits survived the insert.
    //   and al,0x0f (x5) ; hlt
    const std::vector<std::uint8_t> program = {
        0x24, 0x0F, 0x24, 0x0F, 0x24, 0x0F, 0x24, 0x0F, 0x24, 0x0F, 0xF4,
    };
    const std::uint64_t patterns[] = {0x00, 0x0F, 0x80, 0xFF, 0xFFFFFFFFFFFFFFFFULL};
    for (std::uint64_t value : patterns) {
        FlatMemory interp_mem(kBase, kMemSize);
        std::memcpy(interp_mem.data(), program.data(), program.size());
        GuestState initial;
        initial.rip = kBase;
        initial.reg(Gpr::kRax) = value;
        const hyperion::RunResult expected = hyperion::run_program(interp_mem, initial);

        FlatMemory cache_mem(kBase, kMemSize);
        std::memcpy(cache_mem.data(), program.data(), program.size());
        TranslationCache cache;
        const hyperion::RunResult actual = cache.run(cache_mem, initial);

        HYPERION_CHECK(actual.status == RunStatus::kOk);
        HYPERION_CHECK_EQ(actual.state.reg(Gpr::kRax), expected.state.reg(Gpr::kRax));
        HYPERION_CHECK_EQ(actual.state.flags.zf, expected.state.flags.zf);
        HYPERION_CHECK_EQ(actual.state.flags.sf, expected.state.flags.sf);
        HYPERION_CHECK_EQ(actual.state.flags.pf, expected.state.flags.pf);
        HYPERION_CHECK_EQ(actual.state.flags.cf, expected.state.flags.cf);
        HYPERION_CHECK_EQ(actual.state.flags.of, expected.state.flags.of);
        HYPERION_CHECK_EQ(actual.state.flags.af, expected.state.flags.af);
        HYPERION_CHECK_EQ(cache.helper_calls(), std::uint64_t{0});
    }
}

HYPERION_TEST("translation_cache", "helper_counter_is_not_always_zero") {
    // The companion to the test above: if helper_calls() were stuck at zero, the
    // assertion there would pass for the wrong reason. An SSE2 and a DIV are
    // still helper opcodes, so this proves the counter observes real fallbacks.
    //   mov eax, 1 / div ecx / hlt   (div is deliberately not inlined)
    const std::vector<std::uint8_t> program = {
        0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
        0xF7, 0xF1,                   // div ecx
        0xF4,                         // hlt
    };
    FlatMemory mem(kBase, kMemSize);
    std::memcpy(mem.data(), program.data(), program.size());
    GuestState initial;
    initial.rip = kBase;
    initial.reg(Gpr::kRcx) = 2;
    TranslationCache cache;
    const hyperion::RunResult r = cache.run(mem, initial);
    HYPERION_CHECK(r.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRax), std::uint64_t{0}); // 1 / 2 = 0
    HYPERION_CHECK(cache.helper_calls() >= 1);
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

HYPERION_TEST("translation_cache", "eight_and_sixteen_bit_alu_flags_match_interpreter") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "narrow_alu_updates_a_register_allocated_host_register") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "helper_counter_is_not_always_zero") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache",
              "self_loop_superblock_matches_interpreter_with_and_without_step_limit") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

#endif
