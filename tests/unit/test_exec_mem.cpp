// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Executable-memory tests. On an AArch64 host these actually run generated
// code, which is the real proof that the encoder plus the W^X/I-cache path
// works end to end. On a non-AArch64 host they verify the API contract and
// SKIP the execution, rather than pretending a different-ISA test passed.
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/aarch64_encoder.hpp"
#include "hyperion/exec_mem.hpp"

using hyperion::ExecMem;
using hyperion::a64::Encoder;
using hyperion::a64::Reg;
using hyperion::a64::Width;

HYPERION_TEST("exec_mem", "write_then_finalize_then_read_contract") {
    ExecMem mem(64);
    HYPERION_CHECK(mem.size() >= 64ULL);
    const std::uint8_t data[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    mem.write(0, data, 4);
    // Before finalize the region is writable and the bytes are visible.
    HYPERION_CHECK_EQ(mem.writable()[0], 0xAAu);
    mem.finalize();
    // Writing after finalize must be refused, not silently corrupt code.
    bool threw = false;
    try {
        mem.write(0, data, 4);
    } catch (const std::logic_error&) {
        threw = true;
    }
    HYPERION_CHECK(threw);
}

HYPERION_TEST("exec_mem", "entry_requires_finalize") {
    ExecMem mem(64);
    bool threw = false;
    try {
        (void)mem.entry();
    } catch (const std::logic_error&) {
        threw = true;
    }
    HYPERION_CHECK(threw);
}

HYPERION_TEST("exec_mem", "write_past_end_is_rejected") {
    ExecMem mem(64);
    const std::uint8_t data[8] = {};
    bool threw = false;
    try {
        mem.write(mem.size() - 4, data, 8);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    HYPERION_CHECK(threw);

    // Regression: size_t addition overflow in offset + count must also throw
    // out_of_range instead of wrapping around into a small in-bounds value.
    bool overflow_threw = false;
    try {
        mem.write(static_cast<std::size_t>(-4), data, 8);
    } catch (const std::out_of_range&) {
        overflow_threw = true;
    }
    HYPERION_CHECK(overflow_threw);

    bool ctor_overflow_threw = false;
    try {
        ExecMem huge(static_cast<std::size_t>(-1));
        (void)huge;
    } catch (const std::out_of_range&) {
        ctor_overflow_threw = true;
    }
    HYPERION_CHECK(ctor_overflow_threw);
}

HYPERION_TEST("exec_mem", "runs_generated_aarch64_code") {
#if defined(__aarch64__)
    // Generate: add x0, x0, x1 ; ret   -- i.e. f(a, b) = a + b, with the two
    // arguments arriving in x0 and x1 per the AArch64 calling convention.
    Encoder e;
    e.add(Width::k64, 0, 0, 1);
    e.ret();
    const auto code = e.bytes();

    ExecMem mem(code.size());
    mem.write(0, code.data(), code.size());
    mem.finalize();

    using AddFn = std::uint64_t (*)(std::uint64_t, std::uint64_t);
    // Reinterpret the RX region as a function pointer. This is the one place
    // the project converts data to a function pointer, and it is deliberate.
    auto fn = reinterpret_cast<AddFn>(mem.entry());
    HYPERION_CHECK_EQ(fn(2, 3), 5ULL);
    HYPERION_CHECK_EQ(fn(0xFFFFFFFFFFFFFFFFULL, 1ULL), 0ULL); // wraps, as ADD does
#else
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
#endif
}

HYPERION_TEST("exec_mem", "runs_generated_loop") {
#if defined(__aarch64__)
    // Sum 1..n with a loop, exercising a backward branch in generated code:
    //   mov x0, #0        ; acc = 0
    //   mov x3, x0(orig)  ; n  (passed in x0 on entry -> move to x3 first)
    // loop:
    //   cbz x3, done
    //   add x0, x0, x3
    //   sub x3, x3, #1
    //   b loop
    // done:
    //   ret
    // cbz is not in the encoder yet, so use cmp + b.eq instead.
    Encoder e;
    const auto loop = e.new_label();
    const auto done = e.new_label();
    e.mov_reg(Width::k64, 3, 0); // x3 = n
    e.movz(Width::k64, 0, 0);    // x0 = 0 (acc)
    e.bind(loop);
    e.cmp_imm(Width::k64, 3, 0); // compare n with 0
    e.b_cond(hyperion::a64::Cond::kEq, done);
    e.add(Width::k64, 0, 0, 3);     // acc += n
    e.sub_imm(Width::k64, 3, 3, 1); // n -= 1
    e.b(loop);
    e.bind(done);
    e.ret();
    const auto code = e.bytes();

    ExecMem mem(code.size());
    mem.write(0, code.data(), code.size());
    mem.finalize();

    using SumFn = std::uint64_t (*)(std::uint64_t);
    auto fn = reinterpret_cast<SumFn>(mem.entry());
    HYPERION_CHECK_EQ(fn(10), 55ULL); // 1+2+...+10
    HYPERION_CHECK_EQ(fn(0), 0ULL);
    HYPERION_CHECK_EQ(fn(100), 5050ULL);
#else
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
#endif
}

HYPERION_TEST("exec_mem", "arena_write_window_defers_one_mprotect_per_window") {
    // The write window exists so a batch of blocks costs one protection change
    // instead of two per block (audit §29.6: those changes are 99.8% of the cost
    // of publishing a block). This pins the contract the cache relies on: memory
    // handed out inside a window is not executable until publish().
    hyperion::ExecArena arena(4096);
    const std::uint8_t dummy[8] = {0x1F, 0x20, 0x03, 0xD5, 0xC0, 0x03, 0x5F, 0xD6}; // nop; ret

    HYPERION_CHECK_EQ(arena.protection_changes(), std::uint64_t{0});

    arena.begin_write();
    HYPERION_CHECK(arena.write_window_open());
    void* a = arena.allocate(dummy, sizeof(dummy));
    void* b = arena.allocate(dummy, sizeof(dummy));
    HYPERION_CHECK(a != nullptr && b != nullptr);
    HYPERION_CHECK(a != b);
    HYPERION_CHECK_EQ(arena.protection_changes(), std::uint64_t{0}); // still writable
    arena.publish();
    HYPERION_CHECK(!arena.write_window_open());
    HYPERION_CHECK_EQ(arena.protection_changes(), std::uint64_t{1}); // one page, one call

    // A published page can never be written again, so the next window lands on a
    // fresh page and costs exactly one more call - not one per block.
    arena.begin_write();
    void* c = arena.allocate(dummy, sizeof(dummy));
    HYPERION_CHECK(c != nullptr && c != a && c != b);
    arena.publish();
    HYPERION_CHECK_EQ(arena.protection_changes(), std::uint64_t{2});

    // An empty window must not cost a syscall.
    arena.begin_write();
    arena.publish();
    HYPERION_CHECK_EQ(arena.protection_changes(), std::uint64_t{2});
}

#if defined(__aarch64__)

HYPERION_TEST("exec_mem", "arena_published_blocks_are_executable_and_reset_rearms_the_slab") {
    // W^X is only useful if the published pages actually run: a write window that
    // forgot to publish would fault here rather than in a guest.
    hyperion::ExecArena arena(4096);
    const std::uint8_t write_rax[12] = {0x40, 0x05, 0x80, 0xD2,  // mov x0, #42
                                        0xC0, 0x03, 0x5F, 0xD6}; // ret
    using Fn = std::uint64_t (*)();
    arena.begin_write();
    auto* a = reinterpret_cast<Fn>(arena.allocate(write_rax, sizeof(write_rax)));
    arena.publish();
    HYPERION_CHECK_EQ(a(), std::uint64_t{42});

    // reset() must make the surviving slab writable again, otherwise the reuse
    // path would write into read-execute memory and take SIGSEGV.
    const std::uint64_t calls_before = arena.protection_changes();
    arena.reset();
    void* b = arena.allocate(write_rax, sizeof(write_rax));
    HYPERION_CHECK(b != nullptr);
    HYPERION_CHECK_EQ(reinterpret_cast<Fn>(b)(), std::uint64_t{42});
    HYPERION_CHECK(arena.protection_changes() > calls_before);
}

#endif

HYPERION_TEST("exec_mem", "arena_allocates_multiple_wx_blocks_and_reuses_on_reset") {
    hyperion::ExecArena arena(4096);
    const std::uint8_t dummy[8] = {0x1F, 0x20, 0x03, 0xD5, 0xC0, 0x03, 0x5F, 0xD6}; // nop; ret
    void* a = arena.allocate(dummy, sizeof(dummy));
    void* b = arena.allocate(dummy, sizeof(dummy));
    HYPERION_CHECK(a != nullptr && b != nullptr);
    HYPERION_CHECK(a != b);
    arena.reset();
    void* c = arena.allocate(dummy, sizeof(dummy));
    HYPERION_CHECK(c == a);
}
