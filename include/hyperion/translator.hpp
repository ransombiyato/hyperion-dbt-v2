// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// x86-64 -> AArch64 binary translation (standalone & full-system cached JIT).
#ifndef HYPERION_TRANSLATOR_HPP
#define HYPERION_TRANSLATOR_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "hyperion/aarch64_encoder.hpp"
#include "hyperion/decoder.hpp"
#include "hyperion/interpreter.hpp"

namespace hyperion {

enum class TranslateError : std::uint8_t {
    kNone = 0,
    kUnsupportedOpcode,
    kUnsupportedOperand,
    kUnsupportedSize,
    kInternal,
};

const char* to_string(TranslateError error);

struct Translation {
    TranslateError error = TranslateError::kNone;
    std::vector<std::uint8_t> code;
    std::size_t instruction_count = 0;

    bool ok() const { return error == TranslateError::kNone; }
};

// Value returned in x0 by a translated block.
enum class BlockResult : std::uint64_t { // NOLINT(performance-enum-size)
    kOk = 0,
    kHalted = 1,
    kSyscall = 2,
    kFault = 3,
};

enum class BlockExit : std::uint8_t {
    kFallThrough,
    kJmp,
    kJcc,
    kHalted,
    kSyscall,
};

const char* to_string(BlockExit exit);

// An exit site that always jumps to one statically known successor: the byte
// offset of a word emitted as `br x24` (the shared dispatcher) plus the guest PC
// it jumps to. The cache repoints such a word at the successor's entry once that
// entry is known, which removes the hash probe from the transition (audit §26
// row 5). Sites whose target depends on guest data (kJmp reg, kCall, kRet) or on
// a runtime condition (kJcc) are not recorded: one word cannot serve two
// successors, and pretending otherwise would be a miscompile.
struct ExitLink {
    std::uint64_t code_offset = 0; // byte offset of the branch word inside `code`
    std::uint64_t target_pc = 0;   // the successor that word always jumps to
};

struct BlockTranslation {
    TranslateError error = TranslateError::kNone;
    std::vector<std::uint8_t> code;
    std::size_t instruction_count = 0;       // AArch64 instructions emitted
    std::size_t guest_instruction_count = 0; // guest instructions covered

    BlockExit exit = BlockExit::kFallThrough;
    // For kJcc/kSyscall/kFallThrough this is the *fall-through* PC (for kJcc a
    // hint only: rip is authoritative at run time). For kJmp it is the jump
    // destination.
    std::uint64_t target = 0;
    // kJcc only: the branch destination, i.e. the other possible successor.
    // Carried so the cache can translate both sides of a conditional into one
    // write window instead of publishing one block at a time (audit §29.6).
    std::uint64_t alternate = 0;
    bool has_alternate = false;

    // Direct-linkable exit sites, in code order.
    std::vector<ExitLink> exit_links;

    bool ok() const { return error == TranslateError::kNone; }
};

struct RawBlockReturn {
    std::uint64_t status;
    std::uint64_t runs;
};

// Direct-mapped in-JIT block chaining entry (`guest_pc -> host body entry`).
struct DirectJumpEntry {
    std::uint64_t guest_pc = ~0ULL;
    void* body_entry = nullptr;
};
static_assert(sizeof(DirectJumpEntry) == 16);

inline constexpr std::size_t kDirectJumpBits = 12;
inline constexpr std::size_t kDirectJumpSize = std::size_t{1} << kDirectJumpBits;
inline constexpr std::size_t kDirectJumpMask = kDirectJumpSize - 1;

// Execution context pointer held in callee-saved host register `x28` across
// all chained AArch64 basic blocks inside `TranslationCache::run()`.
// Fixed page-aligned sub-table offsets allow 1-instruction `ADD Xd, X28, #k, LSL #12`
// addressing without constant-pool loads.
struct alignas(64) ExecContext {
    void* jit_exit_epilogue = nullptr; // +0
    bool (*mem_read_slow)(ExecContext* ctx, std::uint64_t addr, void* out,
                          std::size_t bytes) noexcept = nullptr; // +8
    bool (*mem_write_slow)(ExecContext* ctx, std::uint64_t addr, const void* in,
                           std::size_t bytes) noexcept = nullptr; // +16
    bool (*exec_complex)(ExecContext* ctx, GuestState* state,
                         const Instruction* insn) noexcept = nullptr; // +24
    Memory* memory = nullptr;                                         // +32
    void* jit_budget_epilogue = nullptr;                              // +40
    std::uint8_t header_pad_[0x1000 - 48]{};

    SoftTlb tlb{};                                      // +0x1000 (read @ +0x1000, write @ +0x3000)
    std::uint8_t tlb_pad_[0x1000 - sizeof(uint64_t)]{}; // align jump_table to +0x6000
    DirectJumpEntry jump_table[kDirectJumpSize]{};      // +0x6000
    // Instructions that fell back to the C++ helper. Diagnostic only, written on
    // the slow path, so it costs nothing in generated code; it exists so
    // "this opcode is inlined now" is an assertion a test can make rather than a
    // claim in a comment.
    std::uint64_t helper_calls = 0;

    void flush_jump_table() noexcept {
        for (auto& e : jump_table) {
            e.guest_pc = ~0ULL;
            e.body_entry = nullptr;
        }
    }
};

static_assert(offsetof(ExecContext, jit_exit_epilogue) == 0);
static_assert(offsetof(ExecContext, mem_read_slow) == 8);
static_assert(offsetof(ExecContext, mem_write_slow) == 16);
static_assert(offsetof(ExecContext, exec_complex) == 24);
static_assert(offsetof(ExecContext, memory) == 32);
static_assert(offsetof(ExecContext, jit_budget_epilogue) == 40);
static_assert(kSoftTlbBits == 9);
static_assert(offsetof(ExecContext, tlb) + offsetof(SoftTlb, read_entries) == 0x1000);
static_assert(offsetof(ExecContext, tlb) + offsetof(SoftTlb, write_entries) == 0x3000);
static_assert(offsetof(ExecContext, jump_table) == 0x6000);

struct JitTrampolines {
    std::vector<std::uint8_t> entry_code;
    std::vector<std::uint8_t> dispatch_code;
    std::vector<std::uint8_t> budget_code;
    std::vector<std::uint8_t> exit_code;
};

// Build shared entry (`RawBlockReturn (*)(GuestState*, uint64_t budget, ExecContext*, void*
// entry)`) and exit epilogue stubs allocated once per `TranslationCache`.
JitTrampolines build_jit_trampolines();

// Full Tier 1 block compiler targeting the shared trampoline ABI + inline
// SoftTlb + in-JIT DirectJumpEntry chaining + self-loop superblocks + full
// baseline x86-64 / SSE2 instruction coverage.
BlockTranslation translate_cached_block(const std::uint8_t* code, std::size_t size,
                                        const Instruction& first);

// Standalone 1-arg / self-loop block translator (preserves all standalone tests).
BlockTranslation translate_block(const std::uint8_t* code, std::size_t size,
                                 const Instruction& first, bool enable_self_loop = false);

Translation translate_instruction(const Instruction& instruction);

} // namespace hyperion

#endif // HYPERION_TRANSLATOR_HPP
