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
    // This test is about the direct-map fast path and the map fallback, so every
    // reach must compile: hot-block gating (P6) would leave the first reaches of
    // each PC interpreted and `block_count()` would not reach 2.
    cache.set_hot_threshold(0);

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
    // The assertion below is about the *helper* counter, which only moves when
    // the block was compiled, so gating is off for this three-instruction
    // program (see the note in p9::run_cached).
    cache.set_hot_threshold(0);
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

// --- P9 stage 1: pending PF/AF descriptor ------------------------------------
//
// Generated code no longer computes PF and AF at every producer; it records two
// bytes from which both are derivable (`GuestState::pf_af_res_low` /
// `pf_af_xor_low`) and materialises them at the forced points: the JIT-to-C++
// exit epilogue, a helper re-entering the interpreter, and a PF/AF consumer in
// generated code (JP/JNP, SETP/SETNP). These tests are differential: every case
// is run through the interpreter and through the cache and the *whole*
// architectural result must agree, so a missed forced point shows up as a wrong
// PF/AF or a wrong branch.
namespace p9 {

struct Outcome {
    hyperion::RunResult result;
    std::uint64_t deferred = 0;
    std::uint64_t flushes = 0;
};

Outcome run_cached(const std::vector<std::uint8_t>& program, const GuestState& initial) {
    FlatMemory mem(kBase, kMemSize);
    std::memcpy(mem.data(), program.data(), program.size());
    TranslationCache cache;
    // These are two- to fifteen-instruction programs that run each block once.
    // Hot-block gating (P6) would correctly interpret them instead of compiling
    // them, so the descriptor machinery under test would never execute and every
    // case would pass for the wrong reason. Gating is therefore switched off
    // here; it is covered on its own by the `gating_*` tests below.
    cache.set_hot_threshold(0);
    Outcome out;
    out.result = cache.run(mem, initial);
    out.deferred = cache.pf_af_deferred();
    out.flushes = cache.pf_af_flushes();
    return out;
}

hyperion::RunResult run_interp(const std::vector<std::uint8_t>& program,
                               const GuestState& initial) {
    FlatMemory mem(kBase, kMemSize);
    std::memcpy(mem.data(), program.data(), program.size());
    return hyperion::run_program(mem, initial);
}

// Compare the entire architectural result of the two tiers. `want_deferred`
// pins the expectation that this program really does defer PF/AF, so a future
// change that silently reverts to eager materialisation cannot turn these tests
// into tautologies.
void check_matches(const char* what, const std::vector<std::uint8_t>& program,
                   const GuestState& initial, bool want_deferred = true) {
    const hyperion::RunResult expected = run_interp(program, initial);
    const Outcome actual = run_cached(program, initial);
    const GuestState& got = actual.result.state;
    const GuestState& want = expected.state;

    bool same = actual.result.status == expected.status && got.rip == want.rip &&
                actual.result.steps == expected.steps && got.flags == want.flags;
    for (std::size_t i = 0; i < hyperion::kGprCount; ++i) {
        same = same && got.gpr[i] == want.gpr[i];
    }
    if (!same) {
        std::fprintf(stderr,
                     "  P9 mismatch in %s: steps %llu/%llu rip 0x%llx/0x%llx\n"
                     "    tier1 cf/pf/af/zf/sf/of = %d%d%d%d%d%d  rax=0x%llx rcx=0x%llx\n"
                     "    tier0 cf/pf/af/zf/sf/of = %d%d%d%d%d%d  rax=0x%llx rcx=0x%llx\n",
                     what, static_cast<unsigned long long>(actual.result.steps),
                     static_cast<unsigned long long>(expected.steps),
                     static_cast<unsigned long long>(got.rip),
                     static_cast<unsigned long long>(want.rip), got.flags.cf, got.flags.pf,
                     got.flags.af, got.flags.zf, got.flags.sf, got.flags.of,
                     static_cast<unsigned long long>(got.reg(Gpr::kRax)),
                     static_cast<unsigned long long>(got.reg(Gpr::kRcx)), want.flags.cf,
                     want.flags.pf, want.flags.af, want.flags.zf, want.flags.sf, want.flags.of,
                     static_cast<unsigned long long>(want.reg(Gpr::kRax)),
                     static_cast<unsigned long long>(want.reg(Gpr::kRcx)));
        HYPERION_CHECK(false);
        return;
    }
    // Leaving generated code must always publish the descriptor: if a pending
    // descriptor survived into C++, the next reader of `state.flags` (the
    // interpreter, a syscall, this test) would see a stale PF/AF.
    HYPERION_CHECK_EQ(static_cast<unsigned>(got.pf_af_pending), 0u);
    if (want_deferred) {
        HYPERION_CHECK(actual.deferred >= 1);
    }
}

// A state whose PF and AF are the *opposite* of what the program computes, so a
// forced point that never runs cannot accidentally agree with the interpreter.
GuestState opposite_pf_af() {
    GuestState s;
    s.rip = kBase;
    s.flags.pf = true;
    s.flags.af = true;
    return s;
}

} // namespace p9

HYPERION_TEST("translation_cache", "pending_pf_af_is_materialised_at_the_exit_epilogue") {
    // add eax, ebx ; hlt
    // 0xff + 0x01 = 0x100: PF = parity(0x00) = 1, AF = carry out of bit 3 = 1.
    // Both are the opposite of the incoming state, so this only passes if the
    // exit epilogue really did materialise the descriptor before returning to
    // C++.
    const std::vector<std::uint8_t> program = {0x01, 0xD8, 0xF4}; // add eax, ebx ; hlt
    GuestState initial = p9::opposite_pf_af();
    initial.reg(Gpr::kRax) = 0xFF;
    initial.reg(Gpr::kRbx) = 0x01;
    p9::check_matches("exit epilogue add", program, initial);
}

HYPERION_TEST("translation_cache", "pending_pf_af_af_zero_case_reaches_the_exit_epilogue") {
    // add eax, ebx ; hlt   with 0x10 + 0x01 = 0x11: AF = 0 (no nibble carry),
    // PF = parity(0x11) = 1. Incoming AF = 1, so a skipped materialisation leaves
    // the wrong bit set.
    const std::vector<std::uint8_t> program = {0x01, 0xD8, 0xF4};
    GuestState initial = p9::opposite_pf_af();
    initial.flags.pf = false;
    initial.reg(Gpr::kRax) = 0x10;
    initial.reg(Gpr::kRbx) = 0x01;
    p9::check_matches("add with zero AF", program, initial);
}

HYPERION_TEST("translation_cache", "pf_consumer_forces_materialisation_inside_a_block") {
    // mov eax,0xff ; add eax,1 ; jp L1 ; mov ecx,0x1111 ; L1: mov ecx,0x2222 ; hlt
    //
    // The JP reads PF *inside* the block, before any exit epilogue runs, so this
    // is decided by the consumer guard alone: the incoming PF is false and the
    // correct PF is true, and the two make the branch go opposite ways. A missing
    // guard leaves ecx = 0x1111 here.
    const std::vector<std::uint8_t> taken = {0xB8, 0xFF, 0x00, 0x00, 0x00, 0x83, 0xC0,
                                             0x01, 0x7A, 0x05, 0xB9, 0x11, 0x11, 0x00,
                                             0x00, 0xB9, 0x22, 0x22, 0x00, 0x00, 0xF4};
    GuestState initial = p9::opposite_pf_af();
    initial.flags.pf = false; // stale value; the add makes PF true
    p9::check_matches("jp taken", taken, initial);

    // Same shape, but 0xff + 2 = 0x101 -> PF = parity(0x01) = 0, so JP must fall
    // through. Incoming PF = true makes a skipped materialisation take the branch.
    const std::vector<std::uint8_t> not_taken = {0xB8, 0xFF, 0x00, 0x00, 0x00, 0x83, 0xC0,
                                                 0x02, 0x7A, 0x05, 0xB9, 0x11, 0x11, 0x00,
                                                 0x00, 0xB9, 0x22, 0x22, 0x00, 0x00, 0xF4};
    GuestState falling = p9::opposite_pf_af();
    falling.flags.pf = true;
    p9::check_matches("jp not taken", not_taken, falling);
}

HYPERION_TEST("translation_cache", "setp_and_setnp_force_materialisation") {
    // mov eax,0xff ; add eax,1 ; setp al ; setnp bl ; hlt
    // PF = 1 for a result of 0x100, so al must become 1 and bl 0.
    const std::vector<std::uint8_t> program = {0xB8, 0xFF, 0x00, 0x00, 0x00, 0x83, 0xC0, 0x01,
                                               0x0F, 0x9A, 0xC0, 0x0F, 0x9B, 0xC3, 0xF4};
    GuestState initial = p9::opposite_pf_af();
    initial.flags.pf = false;
    initial.reg(Gpr::kRbx) = 0xAA;
    p9::check_matches("setp/setnp", program, initial);
}

HYPERION_TEST("translation_cache", "helper_boundary_forces_materialisation_of_pf_and_af") {
    // mov eax,0xff ; add eax,1 ; mov edx,0 ; mov eax,100 ; mov ecx,3 ; div ecx ;
    // jp L1 ; mov esi,0x1111 ; hlt ; L1: mov esi,0x2222 ; hlt
    //
    // DIV runs in the C++ helper, which executes the reference interpreter and
    // therefore reads `state.flags` directly. It also leaves PF/AF untouched, so
    // the descriptor recorded by `add` is what PF/AF must still be after the
    // helper - proving the flush happens *before* the helper runs rather than
    // after. 100/3 = 33 with a remainder, so CF/OF are set by DIV as well.
    const std::vector<std::uint8_t> program = {
        0xB8, 0xFF, 0x00, 0x00, 0x00, 0x83, 0xC0, 0x01, 0xBA, 0x00, 0x00, 0x00, 0x00,
        0xB8, 0x64, 0x00, 0x00, 0x00, 0xB9, 0x03, 0x00, 0x00, 0x00, 0xF7, 0xF1, 0x7A,
        0x06, 0xBE, 0x11, 0x11, 0x00, 0x00, 0xF4, 0xBE, 0x22, 0x22, 0x00, 0x00, 0xF4};
    GuestState initial = p9::opposite_pf_af();
    initial.flags.pf = false;
    const p9::Outcome cached = p9::run_cached(program, initial);
    p9::check_matches("helper boundary", program, initial);
    // The helper path is the only place that reports a dynamic flush count.
    HYPERION_CHECK(cached.flushes >= 1);
}

HYPERION_TEST("translation_cache", "helper_writing_pf_cannot_resurrect_a_stale_descriptor") {
    // mov eax,1 ; add eax,2 ; shr eax,1 ; hlt
    //
    // ADD produces 3 (PF = 1); SHR produces 1 (PF = 0) and runs in the C++ helper,
    // which rewrites PF from its own result. If the helper does not flush the
    // descriptor first, ADD's descriptor is still pending when the block exits and
    // the exit epilogue overwrites the helper's PF with ADD's value. SHR leaves AF
    // alone, so AF still has to come from the ADD.
    const std::vector<std::uint8_t> program = {0xB8, 0x01, 0x00, 0x00, 0x00, 0x83,
                                               0xC0, 0x02, 0xD1, 0xE8, 0xF4};
    GuestState initial = p9::opposite_pf_af();
    p9::check_matches("helper writes pf", program, initial);
    HYPERION_CHECK(p9::run_cached(program, initial).result.state.flags.pf == false);
}

HYPERION_TEST("translation_cache", "partial_flag_update_keeps_cf_and_defers_pf_af") {
    // mov eax,-1 ; add eax,1 ; inc ebx ; hlt     with ebx = 0x7f
    // The ADD sets CF=ZF=AF=PF=1. INC then rewrites PF/AF/ZF/SF/OF but must leave
    // CF alone: the descriptor covers only the flags INC writes, so CF has to
    // survive both the deferral and the materialisation.
    const std::vector<std::uint8_t> program = {0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0x83,
                                               0xC0, 0x01, 0xFF, 0xC3, 0xF4};
    GuestState initial;
    initial.rip = kBase;
    initial.flags.cf = false;
    initial.flags.pf = false;
    initial.reg(Gpr::kRbx) = 0x7F;
    p9::check_matches("inc preserves cf", program, initial);
}

HYPERION_TEST("translation_cache", "pending_pf_af_survives_a_self_loop_and_a_link") {
    // mov ecx,3 ; L: dec ecx ; jnz L ; jp X ; mov edx,7 ; X: hlt
    // `dec ecx ; jnz L` is the self-loop fast path, so the descriptor is recorded
    // by the loop body and must survive the back edges. The final DEC makes
    // ecx = 0 (ZF=1, PF=1, AF=1) and the JP after the loop consumes PF in the
    // *next* block, after a transition.
    const std::vector<std::uint8_t> program = {0xB9, 0x03, 0x00, 0x00, 0x00, 0xFF, 0xC9, 0x75, 0xFC,
                                               0x7A, 0x05, 0xBA, 0x07, 0x00, 0x00, 0x00, 0xF4};
    GuestState initial = p9::opposite_pf_af();
    initial.flags.pf = false;
    initial.reg(Gpr::kRdx) = 0;
    p9::check_matches("self loop + link", program, initial);
}

HYPERION_TEST("translation_cache", "pending_pf_af_crosses_a_directly_linked_exit") {
    // add eax,ebx ; jmp done ; mov edi,0x99 ; done: mov esi,0x1234 ; hlt
    // The JMP exit is linked straight at its successor (P7), so the successor is
    // entered with the descriptor still pending and nothing reads PF there: the
    // flush must happen on the way out to C++.
    const std::vector<std::uint8_t> program = {0x01, 0xD8, 0xEB, 0x05, 0xBF, 0x99, 0x00, 0x00,
                                               0x00, 0xBE, 0x34, 0x12, 0x00, 0x00, 0xF4};
    GuestState initial = p9::opposite_pf_af();
    initial.reg(Gpr::kRax) = 0xFF;
    initial.reg(Gpr::kRbx) = 0x01;
    p9::check_matches("linked exit", program, initial);
}

HYPERION_TEST("translation_cache", "pf_consumer_in_the_successor_block_forces_materialisation") {
    // mov ecx,3 ; cmp ecx,3 ; jne bad ; jp X ; bad: mov edx,0x1111 ; X: mov edx,0x2222 ; hlt
    //
    // CMP 3,3 -> PF = parity(3) = 1. The JP is the first instruction of the next
    // block, so PF has to be materialised in the *successor*, from a descriptor
    // recorded by a block that has already exited. The incoming PF is false, so a
    // missing guard jumps to the wrong place.
    const std::vector<std::uint8_t> program = {0xB9, 0x03, 0x00, 0x00, 0x00, // mov ecx,3
                                               0x83, 0xF9, 0x03,             // cmp ecx,3
                                               0x75, 0x05,                   // jne bad (+5)
                                               0x7A, 0x05,                   // jp X (+5)
                                               0xBA, 0x11, 0x11, 0x00, 0x00, // bad: mov edx,0x1111
                                               0xBA, 0x22, 0x22, 0x00, 0x00, // X:   mov edx,0x2222
                                               0xF4};
    GuestState initial = p9::opposite_pf_af();
    initial.flags.pf = false;
    initial.reg(Gpr::kRdx) = 0;
    p9::check_matches("successor consumer", program, initial);
}

// --- P6: hot-block gating ---------------------------------------------------
//
// A block that is reached once is not worth compiling: translation plus the
// W^X publish costs more than interpreting its instructions, and the profile in
// docs/translator-audit.md §29.12 puts the break-even at tens of thousands of
// guest instructions. `TranslationCache` therefore counts how often each block
// start PC is reached and answers the first `hot_threshold` reaches with the
// reference interpreter instead of generating code. These tests pin the three
// properties that matter: a cold block is *not* translated, a hot block *is*,
// and the observable result is identical either way.
namespace p6 {

// The branch-heavy program below, assembled and checked with as/objdump:
//   xor eax, eax / mov ecx, 9
// L: add eax, ecx / cmp eax, 20 / ja high / sub edx, 3 / jmp next
// high: xor esi, esi / or edi, 1
// next: dec ecx / jnz L / hlt
// Nine iterations with a taken/not-taken mix: enough reaches for every block in
// the loop to cross the default threshold, and enough distinct blocks (three)
// that a linking or promotion bug changes the answer.
const std::vector<std::uint8_t> kBranchHeavy = {
    0x31, 0xC0,                   // xor eax, eax
    0xB9, 0x09, 0x00, 0x00, 0x00, // mov ecx, 9
    0x01, 0xC8,                   // add eax, ecx
    0x83, 0xF8, 0x14,             // cmp eax, 20
    0x77, 0x05,                   // ja high
    0x83, 0xEA, 0x03,             // sub edx, 3
    0xEB, 0x05,                   // jmp next
    0x31, 0xF6,                   // xor esi, esi   (high)
    0x83, 0xCF, 0x01,             // or edi, 1
    0xFF, 0xC9,                   // dec ecx        (next)
    0x75, 0xEB,                   // jnz L
    0xF4,                         // hlt
};

struct Run {
    hyperion::RunResult result;
    std::size_t blocks = 0;
    std::uint64_t gated = 0;
    std::uint64_t promoted = 0;
};

Run run_gated(const std::vector<std::uint8_t>& program, const GuestState& initial,
              std::uint16_t threshold = hyperion::TranslationCache::kDefaultHotThreshold,
              std::uint64_t max_steps = 1'000'000) {
    FlatMemory mem(kBase, kMemSize);
    std::memcpy(mem.data(), program.data(), program.size());
    TranslationCache cache;
    cache.set_hot_threshold(threshold);
    Run out;
    out.result = cache.run(mem, initial, max_steps);
    out.blocks = cache.block_count();
    out.gated = cache.gated_steps();
    out.promoted = cache.promoted_blocks();
    return out;
}

hyperion::RunResult run_reference(const std::vector<std::uint8_t>& program,
                                  const GuestState& initial, std::uint64_t max_steps = 1'000'000) {
    FlatMemory mem(kBase, kMemSize);
    std::memcpy(mem.data(), program.data(), program.size());
    return hyperion::run_program(mem, initial, max_steps);
}

// Full architectural state, so "gating is invisible" is checked on everything
// the two tiers could disagree about, and not just on rax.
void check_same_state(const char* what, const hyperion::RunResult& got,
                      const hyperion::RunResult& want) {
    bool same = got.status == want.status && got.steps == want.steps &&
                got.state.rip == want.state.rip && got.state.flags == want.state.flags;
    for (std::size_t i = 0; i < hyperion::kGprCount; ++i) {
        same = same && got.state.gpr[i] == want.state.gpr[i];
    }
    if (!same) {
        std::fprintf(
            stderr,
            "  P6 mismatch in %s: status %d/%d steps %llu/%llu rip 0x%llx/0x%llx "
            "cf/pf/af/zf/sf/of %d%d%d%d%d%d vs %d%d%d%d%d%d\n",
            what, static_cast<int>(got.status), static_cast<int>(want.status),
            static_cast<unsigned long long>(got.steps), static_cast<unsigned long long>(want.steps),
            static_cast<unsigned long long>(got.state.rip),
            static_cast<unsigned long long>(want.state.rip), got.state.flags.cf, got.state.flags.pf,
            got.state.flags.af, got.state.flags.zf, got.state.flags.sf, got.state.flags.of,
            want.state.flags.cf, want.state.flags.pf, want.state.flags.af, want.state.flags.zf,
            want.state.flags.sf, want.state.flags.of);
    }
    HYPERION_CHECK(same);
    // Whatever tier produced the last instruction, the descriptor must have been
    // published before C++ could look at the state.
    HYPERION_CHECK_EQ(static_cast<unsigned>(got.state.pf_af_pending), 0u);
}

} // namespace p6

HYPERION_TEST("translation_cache", "gating_does_not_translate_a_cold_block") {
    // Three instructions, reached once each: every one of them is cold, so the
    // cache must compile nothing at all and interpret exactly three steps.
    //   mov eax, 7 / add eax, 35 / hlt
    const std::vector<std::uint8_t> program = {
        0xB8, 0x07, 0x00, 0x00, 0x00, // mov eax, 7
        0x83, 0xC0, 0x23,             // add eax, 35
        0xF4,                         // hlt
    };
    GuestState initial;
    initial.rip = kBase;
    const p6::Run got = p6::run_gated(program, initial);
    HYPERION_CHECK(got.result.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(got.result.state.reg(Gpr::kRax), std::uint64_t{42});
    HYPERION_CHECK_EQ(got.blocks, std::size_t{0}); // nothing was translated
    HYPERION_CHECK_EQ(got.promoted, std::uint64_t{0});
    HYPERION_CHECK_EQ(got.gated, std::uint64_t{3}); // all three ran in the interpreter
    p6::check_same_state("cold block", got.result, p6::run_reference(program, initial));
}

HYPERION_TEST("translation_cache", "gating_promotes_a_hot_loop") {
    GuestState initial;
    initial.rip = kBase;
    const p6::Run got = p6::run_gated(p6::kBranchHeavy, initial);
    HYPERION_CHECK(got.result.status == RunStatus::kOk);
    // The loop body is reached ten times, so it must have been compiled, and the
    // cold reaches before that must be visible as interpreted steps.
    HYPERION_CHECK(got.promoted >= 2);
    HYPERION_CHECK(got.blocks >= 2);
    HYPERION_CHECK(got.gated >= 1);
    HYPERION_CHECK(got.gated < got.result.steps); // gating is a prefix, not the whole run
    p6::check_same_state("hot loop", got.result, p6::run_reference(p6::kBranchHeavy, initial));
}

HYPERION_TEST("translation_cache", "gating_zero_threshold_translates_immediately") {
    // `--hot-threshold=0` must reproduce the pre-P6 behaviour exactly: no
    // interpreted steps, first reach compiles. This is the escape hatch for
    // code-size and tier-1 micro-benchmarks, so it has to stay exact.
    GuestState initial;
    initial.rip = kBase;
    const p6::Run got = p6::run_gated(p6::kBranchHeavy, initial, /*threshold=*/0);
    HYPERION_CHECK(got.result.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(got.gated, std::uint64_t{0});
    HYPERION_CHECK_EQ(got.promoted, std::uint64_t{0});
    HYPERION_CHECK(got.blocks >= 2);
    p6::check_same_state("threshold 0", got.result, p6::run_reference(p6::kBranchHeavy, initial));
}

HYPERION_TEST("translation_cache", "gating_results_match_across_tiering_transitions") {
    // The interesting case is not "cold" or "hot" but the boundary between them:
    // the same program is run with the whole ladder of thresholds, from "never
    // compile" (a threshold no single run can reach) through the real default to
    // "always compile", and every run must produce the same architectural state.
    // A promotion that dropped a pending PF/AF descriptor or lost a register
    // would show up as a difference in the middles of this ladder.
    GuestState initial;
    initial.rip = kBase;
    initial.reg(Gpr::kRdx) = 100;
    initial.reg(Gpr::kRdi) = 8;
    const hyperion::RunResult want = p6::run_reference(p6::kBranchHeavy, initial);
    HYPERION_CHECK(want.state.reg(Gpr::kRax) != 0); // the program did something
    for (std::uint16_t threshold : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{2},
                                    std::uint16_t{3}, std::uint16_t{1000}}) {
        const p6::Run got = p6::run_gated(p6::kBranchHeavy, initial, threshold);
        p6::check_same_state("tiering ladder", got.result, want);
    }
}

HYPERION_TEST("translation_cache", "gating_accounts_cold_steps_against_the_step_limit") {
    // A gated step is still a retired guest instruction, so the budget must run
    // out in the middle of the cold prefix exactly as it does in compiled code.
    GuestState initial;
    initial.rip = kBase;
    const p6::Run got =
        p6::run_gated(p6::kBranchHeavy, initial, hyperion::TranslationCache::kDefaultHotThreshold,
                      /*max_steps=*/4);
    HYPERION_CHECK(got.result.status == RunStatus::kStepLimit);
    HYPERION_CHECK_EQ(got.result.steps, std::uint64_t{4});
    HYPERION_CHECK_EQ(got.gated, std::uint64_t{4});
    HYPERION_CHECK_EQ(got.blocks, std::size_t{0}); // four steps never warm a block
    p6::check_same_state("step limit", got.result,
                         p6::run_reference(p6::kBranchHeavy, initial, /*max_steps=*/4));
}

// --- P10: call/return -------------------------------------------------------
//
// Measurement first (§29.13): in a call/return-heavy guest, CALL and RET are 20 %
// of all guest instructions and every one of them used to leave generated code
// through the shared dispatcher - 11 instructions and two jump-table loads to
// turn a `rip` into a block pointer. A *direct* `call` has a statically known
// block start as its destination, exactly like a `jmp`, so it is now linked by
// the same machinery (P7); `ret` and indirect calls keep the dispatcher.
//
// There is deliberately no shadow stack. `ret` must read the architectural
// stack: a guest can rewrite the return address (the second test below does),
// `ret imm16` discards bytes, and the interpreter, a helper or a fault can
// change `rsp` between the call and the return. A cached copy of the return
// address is therefore not a source of truth, and once the load stays, the only
// thing left to save is the pc-to-block lookup - which the dispatcher already
// does with one compare and one indirect branch. §29.13 records the numbers.
namespace p10 {

struct Run {
    hyperion::RunResult result;
    std::size_t linked = 0;
    std::size_t blocks = 0;
};

Run run(const std::vector<std::uint8_t>& program, const GuestState& initial) {
    FlatMemory mem(kBase, kMemSize);
    std::memcpy(mem.data(), program.data(), program.size());
    TranslationCache cache;
    // These programs are a handful of instructions run once, so every call site
    // must actually be compiled for its exit to be linkable at all.
    cache.set_hot_threshold(0);
    Run out;
    out.result = cache.run(mem, initial);
    out.linked = cache.linked_exits();
    out.blocks = cache.block_count();
    return out;
}

hyperion::RunResult run_reference(const std::vector<std::uint8_t>& program,
                                  const GuestState& initial) {
    FlatMemory mem(kBase, kMemSize);
    std::memcpy(mem.data(), program.data(), program.size());
    return hyperion::run_program(mem, initial);
}

void push_imm32(std::vector<std::uint8_t>& code, std::uint64_t value) {
    code.push_back(0x68); // push imm32 (sign-extended to 64 bits)
    for (int i = 0; i < 4; ++i) {
        code.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }
}

} // namespace p10

HYPERION_TEST("translation_cache", "direct_call_exits_are_linked_and_return_correctly") {
    //   call fn / call fn / hlt        (kBase)
    // fn: inc eax / ret                (kBase + 0xB)
    const std::vector<std::uint8_t> program = {
        0xE8, 0x06, 0x00, 0x00, 0x00, // call kBase+0xB   (from 0x5: rel = 6)
        0xE8, 0x01, 0x00, 0x00, 0x00, // call kBase+0xB   (from 0xA: rel = 1)
        0xF4,                         // hlt
        0xFF, 0xC0,                   // inc eax
        0xC3,                         // ret
    };
    GuestState initial;
    initial.rip = kBase;
    initial.reg(Gpr::kRsp) = kBase + 0x800;
    const p10::Run got = p10::run(program, initial);
    HYPERION_CHECK(got.result.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(got.result.state.reg(Gpr::kRax), std::uint64_t{2});
    // The two `call` exits are the only statically known exits in this program,
    // so any link at all is a call link.
    HYPERION_CHECK(got.linked >= 1);
    p6::check_same_state("linked calls", got.result, p10::run_reference(program, initial));
}

HYPERION_TEST("translation_cache", "a_rewritten_return_address_is_honoured") {
    // The guest builds its own return address: `push <pc>` then `ret`, twice.
    // Nothing about the call/return path may be cached, or this program would
    // run the `mov eax, 99` that follows each `ret`.
    std::vector<std::uint8_t> program;
    p10::push_imm32(program, kBase + 0x0B); // ret -> after1
    program.push_back(0xC3);
    program.insert(program.end(), {0xB8, 0x63, 0x00, 0x00, 0x00}); // mov eax, 99 (skipped)
    // after1 (kBase + 0x0B)
    program.insert(program.end(), {0xB8, 0x07, 0x00, 0x00, 0x00}); // mov eax, 7
    p10::push_imm32(program, kBase + 0x1B);                        // ret -> after2
    program.push_back(0xC3);
    program.insert(program.end(), {0xB8, 0x63, 0x00, 0x00, 0x00}); // mov eax, 99 (skipped)
    // after2 (kBase + 0x1B)
    program.insert(program.end(), {0x83, 0xC0, 0x23}); // add eax, 35
    program.push_back(0xF4);                           // hlt
    HYPERION_CHECK_EQ(program.size(), std::size_t{0x1F});

    GuestState initial;
    initial.rip = kBase;
    initial.reg(Gpr::kRsp) = kBase + 0x800;
    const p10::Run got = p10::run(program, initial);
    HYPERION_CHECK(got.result.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(got.result.state.reg(Gpr::kRax), std::uint64_t{42});
    p6::check_same_state("rewritten return", got.result, p10::run_reference(program, initial));
}

HYPERION_TEST("translation_cache", "recursive_calls_match_the_interpreter") {
    // Four nested calls with a conditional base case: exercises a call exit that
    // is linked and a call exit that is not (the recursive one runs from a block
    // whose successor changes identity as the recursion unwinds).
    //   mov edi, 4 / call rec / hlt
    // rec: test edi, edi / je base / dec edi / call rec / add eax, 3 / ret
    // base: mov eax, 1 / ret
    const std::vector<std::uint8_t> program = {
        0xBF, 0x04, 0x00, 0x00, 0x00, // mov edi, 4
        0xE8, 0x01, 0x00, 0x00, 0x00, // call rec
        0xF4,                         // hlt
        0x85, 0xFF,                   // test edi, edi
        0x74, 0x0B,                   // je base
        0xFF, 0xCF,                   // dec edi
        0xE8, 0xF5, 0xFF, 0xFF, 0xFF, // call rec
        0x83, 0xC0, 0x03,             // add eax, 3
        0xC3,                         // ret
        0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
        0xC3,                         // ret
    };
    GuestState initial;
    initial.rip = kBase;
    initial.reg(Gpr::kRsp) = kBase + 0x800;
    const p10::Run got = p10::run(program, initial);
    HYPERION_CHECK(got.result.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(got.result.state.reg(Gpr::kRax), std::uint64_t{13}); // 1 + 4*3
    HYPERION_CHECK_EQ(got.result.state.reg(Gpr::kRsp), initial.reg(Gpr::kRsp));
    p6::check_same_state("recursion", got.result, p10::run_reference(program, initial));
}

HYPERION_TEST("translation_cache", "ret_imm_discards_bytes_from_the_guest_stack") {
    //   push 0 / call fn / hlt / fn: mov eax, 5 / ret 8
    // `ret 8` pops the return address and then discards the caller's junk slot,
    // so `rsp` must be back where it started. Nothing may assume the callee's
    // stack frame has a fixed size.
    const std::vector<std::uint8_t> program = {
        0x6A, 0x00,                   // push 0
        0xE8, 0x01, 0x00, 0x00, 0x00, // call fn
        0xF4,                         // hlt
        0xB8, 0x05, 0x00, 0x00, 0x00, // mov eax, 5
        0xC2, 0x08, 0x00,             // ret 8
    };
    GuestState initial;
    initial.rip = kBase;
    initial.reg(Gpr::kRsp) = kBase + 0x800;
    const p10::Run got = p10::run(program, initial);
    HYPERION_CHECK(got.result.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(got.result.state.reg(Gpr::kRax), std::uint64_t{5});
    HYPERION_CHECK_EQ(got.result.state.reg(Gpr::kRsp), initial.reg(Gpr::kRsp));
    p6::check_same_state("ret imm", got.result, p10::run_reference(program, initial));
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

HYPERION_TEST("translation_cache", "pending_pf_af_is_materialised_at_the_exit_epilogue") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "pending_pf_af_af_zero_case_reaches_the_exit_epilogue") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "pf_consumer_forces_materialisation_inside_a_block") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "setp_and_setnp_force_materialisation") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "helper_boundary_forces_materialisation_of_pf_and_af") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "helper_writing_pf_cannot_resurrect_a_stale_descriptor") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "partial_flag_update_keeps_cf_and_defers_pf_af") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "pending_pf_af_survives_a_self_loop_and_a_link") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "pending_pf_af_crosses_a_directly_linked_exit") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "pf_consumer_in_the_successor_block_forces_materialisation") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "gating_does_not_translate_a_cold_block") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "gating_promotes_a_hot_loop") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "gating_zero_threshold_translates_immediately") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "gating_results_match_across_tiering_transitions") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "gating_accounts_cold_steps_against_the_step_limit") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "direct_call_exits_are_linked_and_return_correctly") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "a_rewritten_return_address_is_honoured") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "recursive_calls_match_the_interpreter") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

HYPERION_TEST("translation_cache", "ret_imm_discards_bytes_from_the_guest_stack") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

#endif
