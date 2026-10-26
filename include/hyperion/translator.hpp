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

// --- P14 stage 1: per-instruction facts --------------------------------------
//
// Translate-time analysis of one decoded guest instruction, computed **once** per
// instruction and consumed by every later pass (register allocation, flag
// liveness, and any future optimisation pass) instead of each pass re-deriving
// the same switches. `compute_ir_facts()` is a pure function of the decoded
// `Instruction`: it reads no machine state, allocates nothing, and cannot fault.
//
// Validity and invalidation
// -------------------------
// The facts describe what the *guest* instruction does, not what the emitter
// chooses to emit, so they stay valid for the whole lifetime of the decoded
// `Instruction` they came from and need no invalidation inside a block:
//
//  * they are recomputed whenever the block is re-decoded, which is the only
//    way the instruction can change (the translator decodes a fresh window on
//    every miss, and code invalidation drops the whole block);
//  * they never outlive the block: nothing caches them across blocks, so a
//    self-modifying-code epoch bump cannot leave a stale fact behind;
//  * `flags_written` is a *superset* view and `flags_killed` the subset that is
//    definitely overwritten (`flags_written == flags_killed` for every opcode
//    today, but the two are kept apart because INC/DEC already differ from the
//    rest and a future pass must not assume equality);
//  * `live_flags` (which of the written flags are actually observed) is *not* a
//    fact: it depends on the whole block and is filled in by the liveness pass
//    from these per-instruction facts.
//
// What is deliberately NOT here: memory ordering, fault points, TLB checks and
// value ranges. Those stay in emission, because an IR pass must never be able to
// reorder or elide a guest load/store across a fault point (audit §29.18).
// Flag bits used by `IrFacts`, numbered the same way as the pending PF/AF
// descriptor in `GuestState` (bit 0 CF, 1 PF, 2 AF, 3 ZF, 4 SF, 5 OF) so one
// value can be used for both.
namespace ir_flags {
inline constexpr std::uint8_t kCf = 1u << 0;
inline constexpr std::uint8_t kPf = 1u << 1;
inline constexpr std::uint8_t kAf = 1u << 2;
inline constexpr std::uint8_t kZf = 1u << 3;
inline constexpr std::uint8_t kSf = 1u << 4;
inline constexpr std::uint8_t kOf = 1u << 5;
inline constexpr std::uint8_t kAll = 0x3F;
} // namespace ir_flags

struct IrFacts {
    // Flags this instruction reads (Jcc/Setcc/Cmovcc conditions, ADC/SBB carry-in,
    // variable shifts that pass flags through on a zero count).
    std::uint8_t flags_read = 0;
    // Flags this instruction overwrites, whether or not anything later reads them.
    std::uint8_t flags_written = 0;
    // Flags this instruction unconditionally overwrites (INC/DEC leave CF alone,
    // MUL/IMUL only write CF and OF, ...). `flags_written` is derived from it.
    std::uint8_t flags_killed = 0;
    // Bit i set = Gpr(i) is read / written by this instruction. A write means the
    // architectural value changes; for partial-register writes the whole 64-bit
    // register is marked because the emitter writes it as a whole.
    std::uint16_t gpr_reads = 0;
    std::uint16_t gpr_writes = 0;
    // Guest memory touched (including the implicit push/pop/call/ret accesses and
    // string operations). A pass may not reorder or elide across these.
    bool reads_memory = false;
    bool writes_memory = false;
    // Control flow ends the block here (Jmp/Jcc/Call/Ret/Hlt/Syscall).
    bool is_terminator = false;
    // Guest instructions retired by executing this instruction (always 1 today;
    // named so the budget accounting reads as "sum of step weights" rather than
    // "number of vector elements").
    std::uint8_t step_weight = 1;
};

// One instruction of a decoded block plus its facts. `live_flags` is filled in by
// the block's backward flag-liveness pass.
struct IrInsn {
    Instruction insn{};
    IrFacts facts{};
    std::uint8_t live_flags = 0;
};

// Compute the facts above for one decoded instruction. Pure and total: every
// opcode, including ones the emitter later rejects, produces a consistent set.
IrFacts compute_ir_facts(const Instruction& insn);

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

    // P9 stage 1: how many PF/AF results this block records as a pending
    // descriptor instead of materialising. Carried out of the translator so a
    // test can assert that the deferral is actually happening, rather than
    // trusting a comment.
    std::uint32_t pf_af_deferred = 0;

    // P9 stage 2: how many flag producers this block records as a pending cc
    // descriptor instead of materialising any flag bytes.
    std::uint32_t cc_deferred = 0;

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
    // Generated stub that calls a C++ slow path with the canonical XMM
    // registers (q16..q23) preserved through GuestState; used by the
    // SoftTlb slow stubs so they stay one word longer than an unguarded
    // call instead of carrying an 8-word spill/reload each.
    void* jit_preserve_call = nullptr; // +48
    // Shared pending-cc materialisation stub (P9 stage 2): one copy in the
    // exit-epilogue code, reached from every in-block flag guard via
    // adr x30 + ldr + br instead of a per-block copy.
    void* jit_cc_materialise = nullptr; // +56
    std::uint8_t header_pad_[0x1000 - 64]{};

    SoftTlb tlb{};                                      // +0x1000 (read @ +0x1000, write @ +0x3000)
    std::uint8_t tlb_pad_[0x1000 - sizeof(uint64_t)]{}; // align jump_table to +0x6000
    DirectJumpEntry jump_table[kDirectJumpSize]{};      // +0x6000
    // Instructions that fell back to the C++ helper. Diagnostic only, written on
    // the slow path, so it costs nothing in generated code; it exists so
    // "this opcode is inlined now" is an assertion a test can make rather than a
    // claim in a comment.
    std::uint64_t helper_calls = 0;
    // Times a live pending PF/AF descriptor had to be materialised at the
    // JIT-to-C++ helper boundary (P9 stage 1). Diagnostic only.
    std::uint64_t pf_af_flushes = 0;

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
static_assert(offsetof(ExecContext, jit_preserve_call) == 48);
static_assert(offsetof(ExecContext, jit_cc_materialise) == 56);
static_assert(kSoftTlbBits == 9);
static_assert(offsetof(ExecContext, tlb) + offsetof(SoftTlb, read_entries) == 0x1000);
static_assert(offsetof(ExecContext, tlb) + offsetof(SoftTlb, write_entries) == 0x3000);
static_assert(offsetof(ExecContext, jump_table) == 0x6000);

struct JitTrampolines {
    std::vector<std::uint8_t> entry_code;
    std::vector<std::uint8_t> dispatch_code;
    std::vector<std::uint8_t> budget_code;
    std::vector<std::uint8_t> exit_code;
    std::vector<std::uint8_t> preserve_code;
    // Byte offset of the shared pending-cc materialisation stub inside
    // exit_code (P9 stage 2); it follows the exit epilogue in that buffer.
    std::size_t cc_materialise_offset = 0;
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
