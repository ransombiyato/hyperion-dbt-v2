// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// x86-64 -> AArch64 translation of the Phase 2 subset (see translator.hpp for
// the exact scope). The generated block has the signature
// `BlockResult block(GuestState*)` and updates the guest state in place.
//
// Design notes:
//   - Guest registers are allocated to callee-saved host registers within a
//     block (Phase 5); see the RegAlloc comment below. GuestState remains
//     authoritative at block boundaries.
//   - x86 flags are *computed*, not delegated: AArch64's ADDS/SUBS set N/Z/C/V,
//     which map onto x86 SF/ZF/CF/OF directly; AF and PF are derived with two
//     ALU identities documented below. This is what makes the translation
//     independently checkable against the interpreter rather than a re-entry
//     into it.
//   - The x8 register holds the GuestState pointer. It is caller-saved in the
//     AAPCS64, so the block needs no prologue/epilogue unless it allocates.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "hyperion/translator.hpp"

namespace hyperion {

namespace {

// AArch64 registers used by a translated block. x8 is the state pointer; the
// rest are scratch. All are caller-saved, so no save/restore is required.
constexpr a64::Reg kState = 8; // GuestState*
constexpr a64::Reg kA = 9;     // operand A (destination value)
constexpr a64::Reg kB = 10;    // operand B (source value)
constexpr a64::Reg kR = 11;    // result
constexpr a64::Reg kT0 = 12;   // scratch
constexpr a64::Reg kT1 = 13;   // scratch
constexpr a64::Reg kT2 = 14;   // scratch
constexpr a64::Reg kT3 = 15;   // scratch

// Guest-register allocation (Phase 5, BENCHMARKS.md §4.2.6).
//
// Within a block, the most-used guest registers are held in callee-saved host
// registers (x19..x28) rather than GuestState memory. Each is loaded once at
// block entry, kept in sync by every instruction that touches it, and spilled
// once at block exit. This lets `add rbx, rax` become a single `adds x_h,
// x_h, x_h` instead of load-load-add-store.
//
// The mapping is a pure fast path: GuestState remains the authoritative home of
// every register at block entry and exit, so the block signature and the
// authoritative-rip rule are unchanged, and a block with no allocation (or a
// guest register beyond the cap) keeps the old memory lowering. Because the
// host registers are callee-saved, they are saved/restored around the block.
constexpr std::size_t kMaxAllocated = 10; // x19..x28
constexpr a64::Reg kAllocBase = 19;

struct RegAlloc {
    std::int8_t host_of_guest[kGprCount];
    RegAlloc() {
        for (auto& h : host_of_guest) {
            h = -1;
        }
    }
    bool mapped(Gpr r) const { return host_of_guest[static_cast<unsigned>(r)] >= 0; }
    a64::Reg host(Gpr r) const {
        return static_cast<a64::Reg>(host_of_guest[static_cast<unsigned>(r)]);
    }
    // The host registers actually in use, in allocation order.
    std::vector<a64::Reg> allocated;
};

// Offsets into GuestState. Generated code hard-codes these, so they are tied
// to the struct with offsetof/static_assert: a layout change becomes a compile
// error here instead of silently corrupting translated code.
constexpr unsigned kRipOffset = offsetof(GuestState, rip);
constexpr unsigned kCfOffset = offsetof(GuestState, flags) + offsetof(Flags, cf);
constexpr unsigned kPfOffset = offsetof(GuestState, flags) + offsetof(Flags, pf);
constexpr unsigned kAfOffset = offsetof(GuestState, flags) + offsetof(Flags, af);
constexpr unsigned kZfOffset = offsetof(GuestState, flags) + offsetof(Flags, zf);
constexpr unsigned kSfOffset = offsetof(GuestState, flags) + offsetof(Flags, sf);
constexpr unsigned kOfOffset = offsetof(GuestState, flags) + offsetof(Flags, of);
// P9 stage 1 pending PF/AF descriptor (see the flag section below).
constexpr unsigned kPfAfPendingOffset = offsetof(GuestState, pf_af_pending);
constexpr unsigned kPfAfResLowOffset = offsetof(GuestState, pf_af_res_low);
constexpr unsigned kPfAfXorLowOffset = offsetof(GuestState, pf_af_xor_low);

static_assert(std::is_trivially_copyable_v<GuestState>,
              "generated code relies on GuestState being a plain memory image");

constexpr unsigned gpr_offset(Gpr r) {
    return 8u * static_cast<unsigned>(r); // 0..120
}

// gpr[] must be first in GuestState for the gpr_offset() arithmetic to hold.
static_assert(offsetof(GuestState, gpr) == 0, "gpr[] must be first in GuestState");
static_assert(offsetof(GuestState, gpr) + 8 * kGprCount == offsetof(GuestState, rip),
              "gpr[] must be immediately followed by rip");

void load_gpr(a64::Encoder& e, Gpr r, a64::Reg dst) {
    e.ldr(a64::Width::k64, dst, kState, gpr_offset(r));
}

void store_gpr(a64::Encoder& e, a64::Reg src, Gpr r) {
    e.str(a64::Width::k64, src, kState, gpr_offset(r));
}

// Register-allocation-aware load/store. If guest register `r` is allocated to a
// host register, the value is copied to/from that register; otherwise it goes
// through GuestState as before.
void load_gpr_rb(a64::Encoder& e, const RegAlloc* ra, Gpr r, a64::Reg dst) {
    if (ra != nullptr && ra->mapped(r)) {
        e.mov_reg(a64::Width::k64, dst, ra->host(r));
    } else {
        load_gpr(e, r, dst);
    }
}

void store_gpr_rb(a64::Encoder& e, const RegAlloc* ra, a64::Reg src, Gpr r) {
    if (ra != nullptr && ra->mapped(r)) {
        e.mov_reg(a64::Width::k64, ra->host(r), src);
    } else {
        store_gpr(e, src, r);
    }
}

// The guest register held in host register `h`, or XZR (31) if none. Used to
// spill an allocated register back to its GuestState slot.
Gpr guest_of_host(const RegAlloc& ra, a64::Reg h) {
    for (unsigned r = 0; r < kGprCount; ++r) {
        if (ra.host_of_guest[r] == static_cast<std::int8_t>(h)) {
            return static_cast<Gpr>(r);
        }
    }
    return static_cast<Gpr>(31);
}

// Parity of the low byte of `src`, into `dst` (0 = odd, 1 = even).
//
// x86 PF is set when the number of set bits in the low byte is even. XOR-folding
// the eight bits gives the odd/even bit; inverting it gives PF. Three shifted
// XORs fold 8 -> 4 -> 2 -> 1 bits, then a UBFX isolates the bit and an XOR with
// 1 inverts it. This replaces a 16-instruction per-bit fold plus its setup (19
// instructions in total) with 6.
void emit_parity(a64::Encoder& e, a64::Reg dst, a64::Reg src) {
    e.eor_shifted(a64::Width::k32, dst, src, src, a64::Shift::kLsr, 4); // b7..b4 ^ b3..b0
    e.eor_shifted(a64::Width::k32, dst, dst, dst, a64::Shift::kLsr, 2);
    e.eor_shifted(a64::Width::k32, dst, dst, dst, a64::Shift::kLsr, 1);
    e.mvn(a64::Width::k32, dst, dst);        // ~parity_bit (leaves NZCV untouched)
    e.ubfx(a64::Width::k32, dst, dst, 0, 1); // PF = (~parity_bit) & 1
}

// --- Flag liveness ----------------------------------------------------------
//
// Which x86 flags an instruction reads and writes. Modelling this lets the
// translator skip computing a flag whose value is overwritten before anything
// reads it, which is most of the emitted code for an ALU instruction.
//
// The subtlety that a naive "every ALU op writes all six flags" model gets
// wrong is INC/DEC: they are ADD/SUB of 1 *with CF held*, so they write
// PF/AF/ZF/SF/OF but leave CF unchanged. flags_written() encodes that, and the
// emission never stores CF for INC/DEC, so CF is preserved for free.
enum FlagBit : std::uint8_t {
    kFbCf = 1u << 0,
    kFbPf = 1u << 1,
    kFbAf = 1u << 2,
    kFbZf = 1u << 3,
    kFbSf = 1u << 4,
    kFbOf = 1u << 5,
};
constexpr std::uint8_t kFbAll = 0x3F;

// Flags an instruction writes. Everything outside the flag-setting subset
// (MOV, NOP, JMP, HLT, ...) writes none.
std::uint8_t flags_written(Opcode op) {
    switch (op) {
    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kCmp:
    case Opcode::kTest:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
        return kFbAll;
    case Opcode::kInc:
    case Opcode::kDec:
        return static_cast<std::uint8_t>(kFbAll & ~kFbCf);
    default:
        return 0;
    }
}

// Flags a condition reads. Must agree with alu::evaluate()/Condition, which the
// differential tests check directly.
std::uint8_t condition_flags(Condition c) {
    switch (c) {
    case Condition::kO:
    case Condition::kNo:
        return kFbOf;
    case Condition::kB:
    case Condition::kAe:
        return kFbCf;
    case Condition::kE:
    case Condition::kNe:
        return kFbZf;
    case Condition::kS:
    case Condition::kNs:
        return kFbSf;
    case Condition::kP:
    case Condition::kNp:
        return kFbPf;
    case Condition::kBe:
    case Condition::kA:
        return static_cast<std::uint8_t>(kFbCf | kFbZf);
    case Condition::kL:
    case Condition::kGe:
        return static_cast<std::uint8_t>(kFbSf | kFbOf);
    case Condition::kLe:
    case Condition::kG:
        return static_cast<std::uint8_t>(kFbSf | kFbOf | kFbZf);
    }
    return 0;
}

// Guest registers an instruction reads, and those it writes. Used both to pick
// which registers to allocate and to price that allocation. Conservative: it
// names every register the instruction could read or write, so the allocation is
// a pure fast path that never changes which registers exist.
void regs_read_write(const Instruction& insn, bool reads[kGprCount], bool writes[kGprCount]) {
    switch (insn.opcode) {
    case Opcode::kMov:
        if (insn.has_src && insn.src.kind == OperandKind::kRegister) {
            reads[static_cast<unsigned>(insn.src.reg)] = true;
        }
        writes[static_cast<unsigned>(insn.dst.reg)] = true;
        break;
    case Opcode::kLea:
        if (insn.has_src && insn.src.kind == OperandKind::kMemory && !insn.src.mem.rip_relative) {
            if (insn.src.mem.has_base) {
                reads[static_cast<unsigned>(insn.src.mem.base)] = true;
            }
            if (insn.src.mem.has_index) {
                reads[static_cast<unsigned>(insn.src.mem.index)] = true;
            }
        }
        writes[static_cast<unsigned>(insn.dst.reg)] = true;
        break;
    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
        reads[static_cast<unsigned>(insn.dst.reg)] = true;
        if (insn.has_src && insn.src.kind == OperandKind::kRegister) {
            reads[static_cast<unsigned>(insn.src.reg)] = true;
        }
        writes[static_cast<unsigned>(insn.dst.reg)] = true;
        break;
    case Opcode::kCmp:
    case Opcode::kTest:
        // Reads both operands, writes no register (only flags).
        reads[static_cast<unsigned>(insn.dst.reg)] = true;
        if (insn.has_src && insn.src.kind == OperandKind::kRegister) {
            reads[static_cast<unsigned>(insn.src.reg)] = true;
        }
        break;
    case Opcode::kInc:
    case Opcode::kDec:
        reads[static_cast<unsigned>(insn.dst.reg)] = true;
        writes[static_cast<unsigned>(insn.dst.reg)] = true;
        break;
    default:
        // Control flow, syscall, hlt, nop: no guest register operand.
        break;
    }
}

// Instructions an allocation of `n` registers adds to the block: the
// save/restore of the borrowed callee-saved host registers (one stp/ldp per
// pair, with an odd register stored as a pair with XZR) plus one ldr in the
// prologue and one str in the epilogue per allocated guest register.
std::size_t alloc_cost(std::size_t n) {
    return 2 * n + 2 * ((n + 1) / 2);
}

// Store the masked flags from the AArch64 condition flags, which must have been
// set by a preceding ADDS (is_sub=false) or SUBS (is_sub=true). `t2` must hold
// (a ^ b ^ result) and is read only when AF is requested; `result` is used for
// parity. Flags outside `mask` are not written, so their previous value stands.
void emit_flags_from_nzcv(a64::Encoder& e, bool is_sub, std::uint8_t mask, a64::Reg t2,
                          a64::Reg result) {
    if (mask & kFbZf) {
        e.cset(a64::Width::k32, kT1, a64::Cond::kEq); // ZF = Z
        e.strb(kT1, kState, kZfOffset);
    }
    if (mask & kFbSf) {
        e.cset(a64::Width::k32, kT1, a64::Cond::kMi); // SF = N
        e.strb(kT1, kState, kSfOffset);
    }
    if (mask & kFbCf) {
        // x86 CF is the carry-out: for add it is "carry set" (CS); for subtract
        // it is "borrow" (CC, i.e. no borrow), the standard ADDS/SUBS mapping.
        e.cset(a64::Width::k32, kT1, is_sub ? a64::Cond::kCc : a64::Cond::kCs);
        e.strb(kT1, kState, kCfOffset);
    }
    if (mask & kFbOf) {
        e.cset(a64::Width::k32, kT1, a64::Cond::kVs); // OF = V
        e.strb(kT1, kState, kOfOffset);
    }
    // PF and AF are *not* materialised here any more: the caller records the
    // pending descriptor with emit_pf_af_store() instead (P9 stage 1). `t2`,
    // which used to carry (a ^ b ^ r) for AF, is now only needed by the caller
    // for that store, and `result` is unused.
    (void)t2;
    (void)result;
}

// Logical ops (AND/OR/XOR) clear CF, OF and AF, and set SF/ZF/PF from the
// result. There is no NZCV involvement.
void emit_flags_logical(a64::Encoder& e, a64::Width w, std::uint8_t mask, a64::Reg result) {
    if (mask & (kFbZf | kFbSf)) {
        e.cmp_imm(w, result, 0); // sets N (sign) and Z from the result
    }
    if (mask & kFbZf) {
        e.cset(a64::Width::k32, kT1, a64::Cond::kEq);
        e.strb(kT1, kState, kZfOffset);
    }
    if (mask & kFbSf) {
        e.cset(a64::Width::k32, kT1, a64::Cond::kMi);
        e.strb(kT1, kState, kSfOffset);
    }
    if (mask & kFbCf) {
        e.strb(a64::kXzr, kState, kCfOffset);
    }
    if (mask & kFbOf) {
        e.strb(a64::kXzr, kState, kOfOffset);
    }
    // PF/AF are deferred; the caller stores the descriptor (AF is statically
    // cleared there by passing a zero xor byte to emit_pf_af_store()).
}

// Exact CF/PF/AF/ZF/SF/OF for an 8- or 16-bit ALU/CMP/TEST, given the flags a
// 32-bit ADDS/SUBS (arithmetic) or ANDS (logical) of the same operands produced.
//
// AArch64 can only set NZCV at 32 or 64 bits wide, so a 32-bit SUBS of two
// zero-extended 8/16-bit operands gives the right CF and ZF but the wrong SF and
// OF: bit 31 is not bit 7, and an 8-bit overflow cannot set 32-bit V. `result`
// therefore has to be the *truncated* result, and SF/OF/AF/PF are computed from
// it directly. Everything here uses non-flag-setting instructions except the
// UBFX after ands/eor (which does not touch NZCV in its non-S form), so the
// NZCV-consuming stores can be emitted in any order.
//
// `result`, `a` and `b` all hold zero-extended w-bit values. Returns nothing:
// like emit_flags_from_nzcv, flags outside `mask` keep their previous value.
void emit_flags_wide(a64::Encoder& e, bool is_logical, bool is_sub, std::uint8_t mask,
                     unsigned bits, a64::Reg result, a64::Reg a, a64::Reg b, a64::Reg t,
                     a64::Reg t2) {
    if (is_logical && (mask & (kFbZf | kFbSf)) != 0) {
        // Logical ops have no flag-setting form here (the encoder has no ANDS);
        // a compare against zero sets N and Z from the result, exactly as
        // emit_flags_logical does for the 32/64-bit case. Z is all that is read
        // back: SF is taken from bit w-1 below, because bit 31 is not bit 7.
        e.cmp_imm(a64::Width::k32, result, 0);
    }
    if (mask & kFbZf) {
        e.cset(a64::Width::k32, t, a64::Cond::kEq); // Z survives truncation
        e.strb(t, kState, kZfOffset);
    }
    if (mask & kFbCf) {
        if (is_logical) {
            e.strb(a64::kXzr, kState, kCfOffset); // logical ops clear CF
        } else {
            e.cset(a64::Width::k32, t, is_sub ? a64::Cond::kCc : a64::Cond::kCs);
            e.strb(t, kState, kCfOffset);
        }
    }
    if (mask & kFbSf) {
        e.ubfx(a64::Width::k32, t, result, bits - 1, 1); // SF is bit w-1, not bit 31
        e.strb(t, kState, kSfOffset);
    }
    if (mask & kFbOf) {
        if (is_logical) {
            e.strb(a64::kXzr, kState, kOfOffset);
        } else {
            // OF is the sign bit of (a ^ r) & (a ^ b) for subtraction and of
            // (a ^ r) & (b ^ r) for addition. At native width AArch64's V flag is
            // already correct, which is why the 32/64-bit path does not need
            // this; at 8/16 bits V comes from a 32-bit operation and cannot see
            // an overflow that does not reach bit 31 (0x00 - 0x80 overflows a
            // byte, not a word). The test that found this is
            // translation_cache.eight_and_sixteen_bit_alu_flags_match_interpreter.
            e.eor(a64::Width::k32, t, a, result);
            e.eor(a64::Width::k32, t2, is_sub ? a : b, is_sub ? b : result);
            e.and_(a64::Width::k32, t, t, t2);
            e.ubfx(a64::Width::k32, t, t, bits - 1, 1);
            e.strb(t, kState, kOfOffset);
        }
    }
    // PF and AF are deferred to the pending descriptor (P9 stage 1): bit 4 of
    // (a ^ b ^ r) and the low byte of the result are width-independent, so the
    // 8/16-bit caller records them with emit_pf_af_store() exactly like the
    // 32/64-bit paths do.
}

// --- Pending PF/AF descriptor (P9 stage 1) ----------------------------------
//
// PF and AF are the two flags with a poor cost/benefit ratio: PF costs a
// five-instruction parity fold, AF three more instructions to derive from
// (a ^ b ^ result), and between them they are read about seventy times less
// often than ZF across the benchmark guests (docs/translator-audit.md §29.10).
// Materialising them eagerly at every producer is therefore almost always work
// whose result is either overwritten or never read.
//
// Instead, a producer stores the two bytes both flags are derivable from:
//
//   pf_af_res_low  = result & 0xFF
//   pf_af_xor_low  = (a ^ b ^ result) & 0xFF   -- bit 4 is AF for add *and* sub,
//                                                 because bit 4 of (a ^ b ^ r) is
//                                                 exactly the carry/borrow into
//                                                 bit 4. Logical ops store 0, as
//                                                 x86 clears AF there.
//
// and sets `pf_af_pending`. Flags are then materialised only at the points that
// can actually observe them:
//
//   1. the shared JIT-to-C++ exit epilogue (`build_jit_trampolines`), which every
//      block exit, budget exhaustion, dispatcher miss and fault path funnels
//      through, so C++ (interpreter, syscalls, tests) never sees a stale flag;
//   2. `jit_exec_complex`, before a helper re-enters the interpreter (C++ side);
//   3. `emit_pf_af_guard`, emitted in front of every PF/AF *consumer* in
//      generated code (JP/JNP, SETP/SETNP).
//
// CF/ZF/SF/OF are deliberately untouched: they are read constantly, so their
// materialisation must stay eager (§29.10 stage 2 is the full descriptor).
//
// Cost per producer: 4 instructions (2 stores, a movz, a store) instead of the
// ~9 that PF+AF used to take. PF/AF are always written as a pair by every
// instruction in this front end, so a mask naming only one of them cannot occur;
// `emit_pf_af_store` handles it anyway by materialising immediately.
constexpr std::uint8_t kFbPfAf = static_cast<std::uint8_t>(kFbPf | kFbAf);

// Where PF and AF live in the pending byte. These are the bit *indices* of the
// two mask bits above, which is what `strb`/`tbz` need: the descriptor uses the
// same numbering as the flag mask so a mask can be stored as a pending mask.
constexpr unsigned kPfAfPendingBitPf = 1;
constexpr unsigned kPfAfPendingBitAf = 2;

// Diagnostics: how many descriptors the block currently being translated
// records. A cache is per-thread and never translates two blocks at once, so a
// thread_local counter is enough; the scope guard below publishes it, which is
// what lets a test assert that PF/AF really are being deferred.
thread_local std::uint32_t tl_pf_af_deferred = 0;

struct PfAfDeferScope {
    BlockTranslation& out;
    std::uint32_t outer;
    explicit PfAfDeferScope(BlockTranslation& target) : out(target), outer(tl_pf_af_deferred) {
        tl_pf_af_deferred = 0;
    }
    ~PfAfDeferScope() {
        out.pf_af_deferred = tl_pf_af_deferred;
        tl_pf_af_deferred = outer;
    }
    PfAfDeferScope(const PfAfDeferScope&) = delete;
    PfAfDeferScope& operator=(const PfAfDeferScope&) = delete;
};

// Materialise the pending flags in `bits` from the descriptor and clear those
// pending bits. Preserves NZCV (only ldrb/strb/eor/mvn/ubfx/and_/movz), so it is
// safe before a `cset` sequence or a fused compare. Clobbers kT2/kT3.
void emit_pf_af_materialise(a64::Encoder& e, std::uint8_t bits, a64::Reg base = kState) {
    if ((bits & kFbPf) != 0) {
        e.ldrb(kT2, base, kPfAfResLowOffset);
        emit_parity(e, kT2, kT2);
        e.strb(kT2, base, kPfOffset);
    }
    if ((bits & kFbAf) != 0) {
        e.ldrb(kT2, base, kPfAfXorLowOffset);
        e.ubfx(a64::Width::k32, kT2, kT2, 4, 1);
        e.strb(kT2, base, kAfOffset);
    }
    if (bits == kFbPfAf) {
        e.strb(a64::kXzr, base, kPfAfPendingOffset);
    } else {
        // Partial clear: pending &= ~bits. Cannot happen for the instructions
        // this front end models (PF and AF are always written together), but
        // keeping it exact means a half-live descriptor can never be exposed.
        e.ldrb(kT2, base, kPfAfPendingOffset);
        e.movz(a64::Width::k32, kT3, static_cast<std::uint16_t>(bits));
        e.mvn(a64::Width::k32, kT3, kT3);
        e.and_(a64::Width::k32, kT2, kT2, kT3);
        e.strb(kT2, base, kPfAfPendingOffset);
    }
}

// Record the pending PF/AF descriptor for the flags in `mask` instead of
// computing them. `result` holds the result (its low byte is enough), and
// `xor_low_value` holds (a ^ b ^ result) -- or `a64::kXzr` when AF is statically
// zero, which is the case for logical ops and for immediate forms whose low
// nibble is zero. Clobbers kT3 (and kT2 in the partial-mask fallback).
void emit_pf_af_store(a64::Encoder& e, std::uint8_t mask, a64::Reg result, a64::Reg xor_low_value,
                      a64::Reg base = kState) {
    const auto bits = static_cast<std::uint8_t>(mask & kFbPfAf);
    if (bits == 0) {
        return;
    }
    ++tl_pf_af_deferred;
    e.strb(result, base, kPfAfResLowOffset);
    e.strb(xor_low_value, base, kPfAfXorLowOffset);
    e.movz(a64::Width::k32, kT3, static_cast<std::uint16_t>(bits));
    e.strb(kT3, base, kPfAfPendingOffset);
    if (bits != kFbPfAf) {
        emit_pf_af_materialise(e, bits, base); // see the note above: unreachable today
    }
}

// Emitted immediately before generated code reads PF/AF. Skips the
// materialisation entirely when nothing is pending, which is the common case
// once the flags have been flushed at an exit. Preserves NZCV.
void emit_pf_af_guard(a64::Encoder& e, std::uint8_t bits, a64::Reg base = kState) {
    bits = static_cast<std::uint8_t>(bits & kFbPfAf);
    if (bits == 0) {
        return;
    }
    const a64::Label done = e.new_label();
    e.ldrb(kT2, base, kPfAfPendingOffset);
    if (bits == kFbPf) {
        e.tbz(kT2, kPfAfPendingBitPf, done);
    } else if (bits == kFbAf) {
        e.tbz(kT2, kPfAfPendingBitAf, done);
    } else {
        e.movz(a64::Width::k32, kT3, static_cast<std::uint16_t>(bits));
        e.and_(a64::Width::k32, kT2, kT2, kT3);
        e.cbz(a64::Width::k32, kT2, done);
    }
    emit_pf_af_materialise(e, bits, base);
    e.bind(done);
}

// Whether `value` fits the AArch64 ADD/SUB 12-bit immediate form (imm12,
// optionally shifted left by 12). If so, fills `imm12`/`shift12`.
bool encode_imm12(std::uint64_t value, std::uint32_t& imm12, bool& shift12) {
    if (value <= 0xFFFu) {
        imm12 = static_cast<std::uint32_t>(value);
        shift12 = false;
        return true;
    }
    if ((value & 0xFFFu) == 0 && (value >> 12) <= 0xFFFu) {
        imm12 = static_cast<std::uint32_t>(value >> 12);
        shift12 = true;
        return true;
    }
    return false;
}

// Load a source operand into `dst`. Only registers and immediates are in scope
// for this increment.
TranslateError load_source(a64::Encoder& e, const Operand& src, a64::Reg dst, int bits,
                           const RegAlloc* ra) {
    switch (src.kind) {
    case OperandKind::kRegister:
        load_gpr_rb(e, ra, src.reg, dst);
        if (bits == 32) {
            // A 32-bit operand use sees only the low 32 bits, and a 32-bit MOV
            // must zero-extend into the destination (the x86-64 rule).
            e.mov_reg(a64::Width::k32, dst, dst);
        }
        return TranslateError::kNone;
    case OperandKind::kImmediate: {
        const std::uint64_t value = (bits == 32) ? (src.imm & 0xFFFFFFFFULL) : src.imm;
        e.mov_imm64(dst, value);
        return TranslateError::kNone;
    }
    case OperandKind::kMemory:
    case OperandKind::kXmm:
        return TranslateError::kUnsupportedOperand;
    }
    return TranslateError::kInternal;
}

void emit_return(a64::Encoder& e, BlockResult result, const RegAlloc* ra = nullptr,
                 bool set_single_run = false) {
    if (ra != nullptr) {
        // Spill every allocated guest register back to its GuestState slot.
        // Spills are individual stores: two allocated host registers are not
        // necessarily consecutive in GuestState, so a pair store would write the
        // wrong slot.
        for (a64::Reg h : ra->allocated) {
            e.str(a64::Width::k64, h, kState, gpr_offset(guest_of_host(*ra, h)));
        }
        // Restore the callee-saved host registers the block borrowed, in the
        // exact reverse of the prologue. An odd count was stored as a pair with
        // XZR, so it is restored first and its second element is discarded.
        const std::size_t npairs = ra->allocated.size() / 2;
        if (ra->allocated.size() % 2 == 1) {
            e.ldp_post(ra->allocated.back(), 31, 31, 16);
        }
        for (std::size_t i = npairs; i > 0; --i) {
            e.ldp_post(ra->allocated[2 * i - 2], ra->allocated[2 * i - 1], 31, 16);
        }
    }
    // Control is about to leave generated code, so any pending PF/AF descriptor
    // has to be materialised before C++ (or the test harness) can read
    // `GuestState::flags`. Same forcing rule as the shared exit epilogue, which
    // this is the direct-`ret` equivalent of.
    emit_pf_af_guard(e, kFbPfAf, kState);
    if (set_single_run) {
        e.movz(a64::Width::k64, 1, 1);
    }
    e.movz(a64::Width::k64, 0, static_cast<std::uint16_t>(result));
    e.ret();
}

// Save the callee-saved host registers a block borrows. Pairs are stored with
// a pre-index writeback; an odd count stores the last alongside XZR.
void emit_prologue(a64::Encoder& e, const RegAlloc& ra) {
    for (std::size_t i = 0; i + 1 < ra.allocated.size(); i += 2) {
        e.stp_pre(ra.allocated[i], ra.allocated[i + 1], 31, -16);
    }
    if (ra.allocated.size() % 2 == 1) {
        e.stp_pre(ra.allocated.back(), 31, 31, -16);
    }
    // Load the allocated guest registers from GuestState into their host
    // registers. This must run after the saves above, which use the stack.
    for (a64::Reg h : ra.allocated) {
        e.ldr(a64::Width::k64, h, kState, gpr_offset(guest_of_host(ra, h)));
    }
}

// Emit `rip = value` for a constant guest address.
void emit_set_rip(a64::Encoder& e, std::uint64_t value) {
    e.mov_imm64(kT0, value);
    e.str(a64::Width::k64, kT0, kState, kRipOffset);
}

// Emit `rip = instruction.address + instruction.length`.
void emit_advance_rip(a64::Encoder& e, const Instruction& insn) {
    emit_set_rip(e, insn.address + insn.length);
}

constexpr a64::Cond invert_cond(a64::Cond c) noexcept {
    return static_cast<a64::Cond>(static_cast<std::uint8_t>(c) ^ 1u);
}

// Map an x86-64 Jcc condition directly to an AArch64 hardware NZCV condition
// code set by an immediately preceding flag-setting arithmetic instruction
// (`ADDS` for ADD/INC; `SUBS` for SUB/CMP/DEC). Returns false when no direct
// hardware condition holds (e.g. PF, or CF read after INC/DEC, or BE/A after
// ADD where `C || Z` is not a single ARMv8 condition).
bool try_nzcv_cond(Opcode prev_op, Condition cond, a64::Cond& out) noexcept {
    if (prev_op != Opcode::kAdd && prev_op != Opcode::kSub && prev_op != Opcode::kCmp &&
        prev_op != Opcode::kInc && prev_op != Opcode::kDec && prev_op != Opcode::kAnd &&
        prev_op != Opcode::kOr && prev_op != Opcode::kXor && prev_op != Opcode::kTest) {
        return false;
    }
    const std::uint8_t req = condition_flags(cond);
    if ((flags_written(prev_op) & req) != req) {
        return false; // e.g. JB/JBE after INC/DEC where CF comes from earlier
    }
    // `SUBS` (emitted for SUB/CMP/DEC and via `CMP R, #0` for AND/OR/XOR/TEST)
    // sets `(N, Z, !C, V) == (SF, ZF, CF, OF)` across all 14 non-parity x86
    // conditions, including `C = 1 == !CF` and `V = 0 == OF` for logical ops.
    const bool is_sub = (prev_op != Opcode::kAdd && prev_op != Opcode::kInc);
    switch (cond) {
    case Condition::kO:
        out = a64::Cond::kVs;
        return true;
    case Condition::kNo:
        out = a64::Cond::kVc;
        return true;
    case Condition::kE:
        out = a64::Cond::kEq;
        return true;
    case Condition::kNe:
        out = a64::Cond::kNe;
        return true;
    case Condition::kS:
        out = a64::Cond::kMi;
        return true;
    case Condition::kNs:
        out = a64::Cond::kPl;
        return true;
    case Condition::kL:
        out = a64::Cond::kLt;
        return true;
    case Condition::kGe:
        out = a64::Cond::kGe;
        return true;
    case Condition::kLe:
        out = a64::Cond::kLe;
        return true;
    case Condition::kG:
        out = a64::Cond::kGt;
        return true;
    case Condition::kB:
        out = is_sub ? a64::Cond::kCc : a64::Cond::kCs;
        return true;
    case Condition::kAe:
        out = is_sub ? a64::Cond::kCs : a64::Cond::kCc;
        return true;
    case Condition::kBe:
        if (!is_sub) {
            return false;
        }
        out = a64::Cond::kLs;
        return true;
    case Condition::kA:
        if (!is_sub) {
            return false;
        }
        out = a64::Cond::kHi;
        return true;
    case Condition::kP:
    case Condition::kNp:
        return false;
    }
    return false;
}

// Branch to `taken` if the x86 condition `c` holds for the modelled flags in
// GuestState, otherwise fall through.
//
// x86 flags live in memory, not in AArch64's NZCV, and several x86 conditions
// (PF; the SF/OF combinations L/GE/LE/G) have no single AArch64 equivalent. So
// rather than map conditions onto NZCV, each condition loads exactly the flag
// bytes it reads, materialises a boolean, and branches on zero/non-zero. This
// is a few more instructions than a clever mapping but is straightforward to
// check against alu::evaluate(), which is the oracle.
void emit_cond_branch(a64::Encoder& e, Condition c, a64::Label taken) {
    auto load = [&](a64::Reg dst, unsigned off) { e.ldrb(dst, kState, off); };
    auto branch_if = [&](a64::Reg src, bool want_nonzero) {
        e.cmp_imm(a64::Width::k32, src, 0);
        e.b_cond(want_nonzero ? a64::Cond::kNe : a64::Cond::kEq, taken);
    };

    switch (c) {
    case Condition::kO:
    case Condition::kNo:
        load(kT0, kOfOffset);
        branch_if(kT0, c == Condition::kO);
        return;
    case Condition::kB:
    case Condition::kAe:
        load(kT0, kCfOffset);
        branch_if(kT0, c == Condition::kB);
        return;
    case Condition::kE:
    case Condition::kNe:
        load(kT0, kZfOffset);
        branch_if(kT0, c == Condition::kE);
        return;
    case Condition::kS:
    case Condition::kNs:
        load(kT0, kSfOffset);
        branch_if(kT0, c == Condition::kS);
        return;
    case Condition::kP:
    case Condition::kNp:
        // PF is only materialised on demand (P9 stage 1), so a consumer has to
        // flush the pending descriptor first. The guard is a single load and a
        // test on the fast path.
        emit_pf_af_guard(e, kFbPf);
        load(kT0, kPfOffset);
        branch_if(kT0, c == Condition::kP);
        return;
    case Condition::kBe:
    case Condition::kA: {
        load(kT0, kCfOffset);
        load(kT1, kZfOffset);
        e.orr(a64::Width::k32, kT2, kT0, kT1); // kT2 = CF | ZF
        branch_if(kT2, c == Condition::kBe);
        return;
    }
    case Condition::kL:
    case Condition::kGe: {
        load(kT0, kSfOffset);
        load(kT1, kOfOffset);
        e.eor(a64::Width::k32, kT2, kT0, kT1); // kT2 = SF ^ OF
        branch_if(kT2, c == Condition::kL);
        return;
    }
    case Condition::kLe:
    case Condition::kG: {
        load(kT0, kSfOffset);
        load(kT1, kOfOffset);
        load(kT2, kZfOffset);
        e.eor(a64::Width::k32, kT3, kT0, kT1); // kT3 = SF ^ OF
        e.orr(a64::Width::k32, kT3, kT3, kT2); // kT3 = (SF ^ OF) | ZF
        branch_if(kT3, c == Condition::kLe);
        return;
    }
    }
}

} // namespace

const char* to_string(BlockExit exit) {
    switch (exit) {
    case BlockExit::kFallThrough:
        return "fall-through";
    case BlockExit::kJmp:
        return "jmp";
    case BlockExit::kJcc:
        return "jcc";
    case BlockExit::kHalted:
        return "halted";
    case BlockExit::kSyscall:
        return "syscall";
    }
    return "?";
}

// Emit the AArch64 for one already-validated instruction into `e`. `e` must
// already have x8 = GuestState*. Returns a translate error, or kNone on
// success. Control-flow instructions are handled by the block translator, not
// here, so they are rejected as unsupported by this helper.
namespace {

TranslateError emit_one(a64::Encoder& e, const Instruction& insn, bool write_rip,
                        std::uint8_t live_flags, const RegAlloc* ra = nullptr) {
    const bool is32 = insn.dst.size == OperandSize::k32;
    const a64::Width w = is32 ? a64::Width::k32 : a64::Width::k64;
    const int bits = is32 ? 32 : 64;

    switch (insn.opcode) {
    case Opcode::kNop:
        if (write_rip) {
            emit_advance_rip(e, insn);
        }
        return TranslateError::kNone;

    case Opcode::kHlt:
        if (write_rip) {
            emit_set_rip(e, insn.address);
        }
        emit_return(e, BlockResult::kHalted, ra);
        return TranslateError::kNone;

    case Opcode::kSyscall:
        // A syscall ends the block: the dispatcher must run the handler (which
        // needs the Memory object, unavailable in generated code) and then
        // resume. rip is advanced here so the handler, and any resume, sees the
        // instruction after `syscall`, matching a real CPU.
        emit_set_rip(e, insn.address + insn.length);
        emit_return(e, BlockResult::kSyscall, ra);
        return TranslateError::kNone;

    case Opcode::kMov: {
        // If both operands are allocated, the move is a single host-register
        // move (the 32-bit form zero-extends, matching x86-64). If the source is
        // an immediate and the destination is allocated, materialise straight
        // into the host register. Otherwise fall back to the memory path.
        if (ra != nullptr && ra->mapped(insn.dst.reg) && insn.src.kind == OperandKind::kRegister &&
            ra->mapped(insn.src.reg)) {
            e.mov_reg(w, ra->host(insn.dst.reg), ra->host(insn.src.reg));
        } else if (ra != nullptr && ra->mapped(insn.dst.reg) &&
                   insn.src.kind == OperandKind::kImmediate) {
            const std::uint64_t value =
                (bits == 32) ? (insn.src.imm & 0xFFFFFFFFULL) : insn.src.imm;
            e.mov_imm64(ra->host(insn.dst.reg), value);
        } else {
            const TranslateError err = load_source(e, insn.src, kA, bits, ra);
            if (err != TranslateError::kNone) {
                return err;
            }
            store_gpr_rb(e, ra, kA, insn.dst.reg);
        }
        if (write_rip) {
            emit_advance_rip(e, insn);
        }
        return TranslateError::kNone;
    }

    case Opcode::kLea: {
        const a64::Reg dst =
            (ra != nullptr && ra->mapped(insn.dst.reg)) ? ra->host(insn.dst.reg) : kR;
        if (insn.src.mem.rip_relative) {
            std::uint64_t val =
                (insn.address + insn.length) + static_cast<std::uint64_t>(insn.src.mem.disp);
            if (bits == 32) {
                val &= 0xFFFFFFFFULL;
            }
            e.mov_imm64(dst, val);
        } else {
            bool have_acc = false;
            if (insn.src.mem.has_base) {
                if (ra != nullptr && ra->mapped(insn.src.mem.base)) {
                    e.mov_reg(a64::Width::k64, kT0, ra->host(insn.src.mem.base));
                } else {
                    load_gpr(e, insn.src.mem.base, kT0);
                }
                have_acc = true;
            }
            if (insn.src.mem.has_index) {
                a64::Reg idx = kT1;
                if (ra != nullptr && ra->mapped(insn.src.mem.index)) {
                    idx = ra->host(insn.src.mem.index);
                } else {
                    load_gpr(e, insn.src.mem.index, kT1);
                }
                unsigned sh = 0;
                switch (insn.src.mem.scale) {
                case 2:
                    sh = 1;
                    break;
                case 4:
                    sh = 2;
                    break;
                case 8:
                    sh = 3;
                    break;
                default:
                    sh = 0;
                    break;
                }
                e.add_shifted(a64::Width::k64, kT0, have_acc ? kT0 : a64::kXzr, idx,
                              a64::Shift::kLsl, sh);
                have_acc = true;
            }
            if (insn.src.mem.disp != 0) {
                const auto disp_u64 = static_cast<std::uint64_t>(insn.src.mem.disp);
                std::uint32_t imm12 = 0;
                bool shift12 = false;
                if (!have_acc) {
                    e.mov_imm64(kT0, disp_u64);
                    have_acc = true;
                } else if (insn.src.mem.disp > 0 && encode_imm12(disp_u64, imm12, shift12)) {
                    e.add_imm(a64::Width::k64, kT0, kT0, imm12, shift12);
                } else if (insn.src.mem.disp < 0 && encode_imm12(0ULL - disp_u64, imm12, shift12)) {
                    e.sub_imm(a64::Width::k64, kT0, kT0, imm12, shift12);
                } else {
                    e.mov_imm64(kT1, disp_u64);
                    e.add(a64::Width::k64, kT0, kT0, kT1);
                }
            }
            if (!have_acc) {
                e.movz(a64::Width::k64, dst, 0);
            } else {
                e.mov_reg(w, dst, kT0);
            }
        }
        if (dst == kR) {
            store_gpr_rb(e, ra, kR, insn.dst.reg);
        }
        if (write_rip) {
            emit_advance_rip(e, insn);
        }
        return TranslateError::kNone;
    }

    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
    case Opcode::kCmp:
    case Opcode::kTest: {
        // Operate directly on the allocated host registers when both operands
        // are allocated; kA/kB/kR are used only as fallbacks. This is what turns
        // `add rdx, rax` into one `adds x, x, x` instead of load/load/add/store.
        const a64::Reg A =
            (ra != nullptr && ra->mapped(insn.dst.reg)) ? ra->host(insn.dst.reg) : kA;
        if (A == kA) {
            load_gpr(e, insn.dst.reg, kA);
        }
        const a64::Reg B = (ra != nullptr && insn.has_src &&
                            insn.src.kind == OperandKind::kRegister && ra->mapped(insn.src.reg))
                               ? ra->host(insn.src.reg)
                               : kB;
        // CMP and TEST do not write a destination register; their result is
        // computed solely for flags.
        const bool writes_dst = insn.opcode != Opcode::kCmp && insn.opcode != Opcode::kTest;
        const a64::Reg R =
            (ra != nullptr && writes_dst && ra->mapped(insn.dst.reg)) ? ra->host(insn.dst.reg) : kR;

        const bool is_logical = insn.opcode == Opcode::kAnd || insn.opcode == Opcode::kOr ||
                                insn.opcode == Opcode::kXor || insn.opcode == Opcode::kTest;
        const bool is_sub = insn.opcode == Opcode::kSub || insn.opcode == Opcode::kCmp;

        // Only flags that are written *and* read before being overwritten are
        // worth computing. An empty mask means no flag is live, so the whole
        // flag prologue disappears.
        const std::uint8_t mask =
            static_cast<std::uint8_t>(flags_written(insn.opcode) & live_flags);

        // An immediate source that fits the AArch64 ADD/SUB immediate form is
        // folded directly into the operation instead of materialising kB first.
        // Even when AF is live, AF depends only on the low 4 bits of the two
        // operands:
        //   - ADD: AF = ((a & 0xF) + (imm & 0xF)) >= 16
        //   - SUB/CMP: AF = (a & 0xF) < (imm & 0xF)
        // Notably, when (imm & 0xF) == 0 (including all shift12 multiples of
        // 0x1000), AF is identically 0 for every guest operand value.
        std::uint32_t imm12 = 0;
        bool shift12 = false;
        const std::uint64_t imm_value =
            insn.has_src ? ((bits == 32) ? (insn.src.imm & 0xFFFFFFFFULL) : insn.src.imm) : 0ULL;
        const bool imm_folded = insn.has_src && insn.src.kind == OperandKind::kImmediate &&
                                (insn.opcode == Opcode::kAdd || insn.opcode == Opcode::kSub ||
                                 insn.opcode == Opcode::kCmp) &&
                                encode_imm12(imm_value, imm12, shift12);

        if (!imm_folded && B == kB) {
            const TranslateError err = load_source(e, insn.src, kB, bits, ra);
            if (err != TranslateError::kNone) {
                return err;
            }
        }

        if (is_logical) {
            switch (insn.opcode) {
            case Opcode::kAnd:
            case Opcode::kTest:
                e.and_(w, R, A, B);
                break;
            case Opcode::kOr:
                e.orr(w, R, A, B);
                break;
            default:
                e.eor(w, R, A, B);
                break;
            }
            if (mask != 0) {
                emit_flags_logical(e, w, mask, R);
                // Logical ops define AF as 0, so a zero xor byte is exact.
                emit_pf_af_store(e, mask, R, a64::kXzr);
            }
        } else if (imm_folded) {
            const auto imm_nibble = static_cast<std::uint32_t>(imm_value & 0xFULL);
            const bool pfaf_live = (mask & kFbPfAf) != 0;
            // AF = bit 4 of (a ^ imm ^ r). When the immediate's low nibble is
            // zero, no carry can enter bit 4, so AF is identically 0 and the
            // descriptor's xor byte can be the zero register.
            const bool af_from_xor = pfaf_live && imm_nibble != 0;
            a64::Reg prev_a = A;
            if (af_from_xor && R == A) {
                e.mov_reg(a64::Width::k64, kT2, A);
                prev_a = kT2;
            }
            if (is_sub) {
                e.subs_imm(w, R, A, imm12, shift12);
            } else {
                e.adds_imm(w, R, A, imm12, shift12);
            }
            if (af_from_xor) {
                // xor byte = a ^ r, with bit 4 toggled when bit 4 of the
                // immediate is set. Complementing the whole word is the cheap
                // way to do that: only bit 4 of the xor byte is read back.
                e.eor(a64::Width::k64, kT2, prev_a, R);
                if ((imm_value & 0x10ULL) != 0) {
                    e.mvn(a64::Width::k32, kT2, kT2);
                }
            }
            const auto eager_mask = static_cast<std::uint8_t>(mask & ~kFbPfAf);
            if (eager_mask != 0) {
                emit_flags_from_nzcv(e, is_sub, eager_mask, kT2, R);
            }
            emit_pf_af_store(e, mask, R, af_from_xor ? kT2 : a64::kXzr);
        } else {
            const bool pfaf_live = (mask & kFbPfAf) != 0;
            if (pfaf_live) {
                // t2 = a ^ b, completed to (a ^ b ^ r) after the op: bit 4 is AF.
                e.eor(a64::Width::k64, kT2, A, B);
            }
            if (is_sub) {
                e.subs(w, R, A, B);
            } else {
                e.adds(w, R, A, B);
            }
            if (pfaf_live) {
                e.eor(a64::Width::k64, kT2, kT2, R);
            }
            const auto eager_mask = static_cast<std::uint8_t>(mask & ~kFbPfAf);
            if (eager_mask != 0) {
                emit_flags_from_nzcv(e, is_sub, eager_mask, kT2, R);
            }
            emit_pf_af_store(e, mask, R, kT2);
        }

        if (writes_dst && R == kR) {
            // The result landed in the scratch register, so it must be written
            // back. When R is the destination's own host register it is already
            // in place.
            store_gpr_rb(e, ra, kR, insn.dst.reg);
        }
        if (write_rip) {
            emit_advance_rip(e, insn);
        }
        return TranslateError::kNone;
    }

    case Opcode::kInc:
    case Opcode::kDec: {
        // INC/DEC are ADD/SUB of immediate #1 with CF preserved. Using
        // ADDS_IMM/SUBS_IMM #1 avoids materialising 1 in a register and leaves
        // hardware NZCV valid for direct fusion with a following Jcc.
        const a64::Reg A =
            (ra != nullptr && ra->mapped(insn.dst.reg)) ? ra->host(insn.dst.reg) : kA;
        if (A == kA) {
            load_gpr(e, insn.dst.reg, kA);
        }
        const bool is_sub = insn.opcode == Opcode::kDec;
        const std::uint8_t mask =
            static_cast<std::uint8_t>(flags_written(insn.opcode) & live_flags);
        const a64::Reg R =
            (ra != nullptr && ra->mapped(insn.dst.reg)) ? ra->host(insn.dst.reg) : kR;
        const bool pfaf_live = (mask & kFbPfAf) != 0;
        a64::Reg prev_a = A;
        if (pfaf_live && R == A) {
            e.mov_reg(a64::Width::k64, kT2, A);
            prev_a = kT2;
        }
        if (is_sub) {
            e.subs_imm(w, R, A, 1);
        } else {
            e.adds_imm(w, R, A, 1);
        }
        if (pfaf_live) {
            e.eor(a64::Width::k64, kT2, prev_a, R); // bit 4 = AF
        }
        const auto eager_mask = static_cast<std::uint8_t>(mask & ~kFbPfAf);
        if (eager_mask != 0) {
            emit_flags_from_nzcv(e, is_sub, eager_mask, kT2, R);
        }
        emit_pf_af_store(e, mask, R, kT2);
        if (R == kR) {
            store_gpr_rb(e, ra, kR, insn.dst.reg);
        }
        if (write_rip) {
            emit_advance_rip(e, insn);
        }
        return TranslateError::kNone;
    }

    case Opcode::kJmp:
    case Opcode::kJcc:
    case Opcode::kPush:
    case Opcode::kPop:
    case Opcode::kRet:
    default:
        return TranslateError::kUnsupportedOpcode;
    }
    return TranslateError::kInternal;
}

// Whether the block translator may include this instruction. Control flow is
// handled by the block driver; PUSH/POP need a guest stack (Phase 6) and are
// cut, so the dispatcher stops at them rather than miscompiling.
bool is_block_body_opcode(Opcode op) {
    switch (op) {
    case Opcode::kMov:
    case Opcode::kLea:
    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kCmp:
    case Opcode::kTest:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
    case Opcode::kInc:
    case Opcode::kDec:
    case Opcode::kNop:
        return true;
    default:
        return false;
    }
}

// Validate the operand shape of a body instruction, matching translate_instruction.
TranslateError validate_body(const Instruction& insn) {
    if (insn.dst.size != OperandSize::k32 && insn.dst.size != OperandSize::k64) {
        return TranslateError::kUnsupportedSize;
    }
    if (insn.opcode == Opcode::kLea) {
        if (insn.dst.kind != OperandKind::kRegister) {
            return TranslateError::kInternal;
        }
        if (!insn.has_src || insn.src.kind != OperandKind::kMemory) {
            return TranslateError::kUnsupportedOperand;
        }
        return TranslateError::kNone;
    }
    if (insn.dst.kind == OperandKind::kMemory) {
        return TranslateError::kUnsupportedOperand;
    }
    if (insn.dst.kind != OperandKind::kRegister) {
        return TranslateError::kInternal;
    }
    if (insn.has_src && insn.src.kind == OperandKind::kMemory) {
        return TranslateError::kUnsupportedOperand;
    }
    return TranslateError::kNone;
}

} // namespace

const char* to_string(TranslateError error) {
    switch (error) {
    case TranslateError::kNone:
        return "none";
    case TranslateError::kUnsupportedOpcode:
        return "unsupported opcode";
    case TranslateError::kUnsupportedOperand:
        return "unsupported operand";
    case TranslateError::kUnsupportedSize:
        return "unsupported operand size";
    case TranslateError::kInternal:
        return "internal error";
    }
    return "?";
}

Translation translate_instruction(const Instruction& insn) {
    Translation out;
    a64::Encoder e;
    e.mov_reg(a64::Width::k64, kState, 0); // x8 = GuestState*

    // Control flow is not meaningful for a lone instruction: a standalone block
    // cannot branch anywhere. Refuse it here; translate_block() handles it.
    if (insn.opcode == Opcode::kJmp || insn.opcode == Opcode::kJcc || insn.opcode == Opcode::kRet ||
        insn.opcode == Opcode::kPush || insn.opcode == Opcode::kPop) {
        out.error = TranslateError::kUnsupportedOpcode;
        return out;
    }

    if (insn.opcode != Opcode::kNop && insn.opcode != Opcode::kHlt &&
        insn.opcode != Opcode::kSyscall) {
        const TranslateError err = validate_body(insn);
        if (err != TranslateError::kNone) {
            out.error = err;
            return out;
        }
    }

    const TranslateError err = emit_one(e, insn, /*write_rip=*/true, kFbAll);
    if (err != TranslateError::kNone) {
        out.error = err;
        return out;
    }

    if (insn.opcode != Opcode::kHlt && insn.opcode != Opcode::kSyscall) {
        emit_return(e, BlockResult::kOk);
    }

    out.code = e.bytes();
    out.instruction_count = e.count();
    return out;
}

BlockTranslation translate_block(const std::uint8_t* code, std::size_t size,
                                 const Instruction& first, bool enable_self_loop) {
    BlockTranslation out;

    // Pass 1: decode the block into a straight-line list of instructions. This
    // has to happen before emission so flag liveness can be computed backward
    // over the whole block (pass 2); the emitter (pass 3) then knows, for each
    // instruction, which flags are actually read before being overwritten.
    struct Step {
        Instruction insn;
        std::uint8_t live_flags = 0;
    };
    std::vector<Step> steps;

    // `code`/`size` start at `first.address`, so an instruction at guest address
    // A sits at byte offset A - first.address.
    Instruction insn = first;
    std::size_t next_offset = first.length;
    BlockExit exit = BlockExit::kFallThrough;
    std::uint64_t target = 0;
    std::uint64_t alternate = 0; // kJcc branch destination, for successor prefetch

    while (true) {
        const Opcode op = insn.opcode;

        if (op == Opcode::kHlt || op == Opcode::kRet) {
            // Both stop the program. RET is modelled by the interpreter as a
            // top-level return (there is no guest stack yet, Phase 6).
            steps.push_back({insn});
            exit = BlockExit::kHalted;
            break;
        }
        if (op == Opcode::kSyscall) {
            // End the block at the syscall so the dispatcher can run the
            // handler. rip is advanced past the instruction, so a resume (or the
            // next block lookup) continues correctly.
            steps.push_back({insn});
            exit = BlockExit::kSyscall;
            target = insn.address + insn.length;
            break;
        }
        if (op == Opcode::kJmp) {
            steps.push_back({insn});
            exit = BlockExit::kJmp;
            target = insn.dst.imm;
            break;
        }
        if (op == Opcode::kJcc) {
            steps.push_back({insn});
            exit = BlockExit::kJcc;
            target = insn.address + insn.length; // hint only; rip is authoritative
            alternate = insn.dst.imm;
            break;
        }

        // Not a terminator. If we can translate it, add it and continue.
        // Otherwise cut the block *before* it: the dispatcher then sees the
        // unsupported PC on the next lookup and stops, instead of running an
        // instruction we did not translate.
        //
        // The cut is only valid once the block covers at least one instruction.
        // If the *first* instruction is unsupported there is nothing to cut
        // back to, and an empty block would fall through to the same PC and loop
        // forever, so we report the error and let the caller stop.
        if (!is_block_body_opcode(op) || validate_body(insn) != TranslateError::kNone) {
            if (steps.empty()) {
                out.error = TranslateError::kUnsupportedOpcode;
                return out;
            }
            exit = BlockExit::kFallThrough;
            target = insn.address;
            break;
        }
        steps.push_back({insn});

        if (next_offset >= size) {
            exit = BlockExit::kFallThrough;
            target = first.address + next_offset;
            break;
        }
        const DecodeResult next =
            decode(code + next_offset, size - next_offset, first.address + next_offset);
        if (!next.ok()) {
            exit = BlockExit::kFallThrough;
            target = first.address + next_offset;
            break;
        }
        insn = next.instruction;
        next_offset += insn.length;
    }

    // Pass 1.5: choose which guest registers to keep in host registers for this
    // block. Without allocation every source reference emits one `ldr` from
    // GuestState and every write emits one `str`; with allocation all of those
    // disappear from the body, at the cost of one `ldr` (prologue) and one `str`
    // (epilogue) per allocated register plus the save/restore of the borrowed
    // host registers. A register is worth a slot only when its body references
    // outnumber that overhead; see alloc_cost().
    std::size_t refs[kGprCount] = {};
    for (const Step& step : steps) {
        bool r[kGprCount] = {};
        bool w[kGprCount] = {};
        regs_read_write(step.insn, r, w);
        for (unsigned i = 0; i < kGprCount; ++i) {
            refs[i] += static_cast<std::size_t>((r[i] ? 1 : 0) + (w[i] ? 1 : 0));
        }
    }

    RegAlloc ra;
    // Take the registers with the largest saving first and stop as soon as the
    // marginal register no longer covers the step increase in overhead, so a
    // block is allocated only when it nets positive.
    std::vector<unsigned> order;
    for (unsigned i = 0; i < kGprCount; ++i) {
        if (refs[i] > 0) {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
        return refs[a] != refs[b] ? refs[a] > refs[b] : a < b;
    });
    std::size_t total_saving = 0;
    std::size_t nalloc = 0;
    while (nalloc < order.size() && nalloc < kMaxAllocated) {
        const std::size_t next = nalloc + 1;
        const std::size_t gain = refs[order[nalloc]];
        if (total_saving + gain < alloc_cost(next)) {
            break;
        }
        total_saving += gain;
        ++nalloc;
    }
    for (std::size_t i = 0; i < nalloc; ++i) {
        const a64::Reg h = static_cast<a64::Reg>(kAllocBase + i);
        ra.host_of_guest[order[i]] = static_cast<std::int8_t>(h);
        ra.allocated.push_back(h);
    }

    // Pass 2: backward flag liveness. Flags live in GuestState and persist
    // across blocks, so at a block exit every flag is conservatively live: a
    // successor block may read any of them, and there is no cross-block
    // analysis. (This is sound because the invariant "all flags are correct at
    // block entry" then holds by induction from program start, where both tiers
    // begin with the same zeroed flags.)
    //
    // live_out = flags that must be correct just after this instruction.
    // live_in  = reads | (live_out & ~writes) = flags that must be correct on
    //            entry. A flag the instruction writes is *computed* only if it
    //            is also live_out; a flag it does not write is merely preserved
    //            (never stored), which is exactly how INC/DEC keep CF.
    std::uint8_t live_out = kFbAll;
    for (std::size_t i = steps.size(); i-- > 0;) {
        const Instruction& in = steps[i].insn;
        const std::uint8_t reads =
            (in.opcode == Opcode::kJcc) ? condition_flags(in.condition) : std::uint8_t{0};
        const std::uint8_t writes = flags_written(in.opcode);
        steps[i].live_flags = static_cast<std::uint8_t>(writes & live_out);
        live_out = static_cast<std::uint8_t>(reads | (live_out & ~writes));
    }

    // Pass 3: emit. Body instructions do not write rip: nothing inside a block
    // reads it, so the per-instruction store is dead. The block writes rip once,
    // at its exit. See BENCHMARKS.md §4.2.2.
    a64::Encoder e;
    e.mov_reg(a64::Width::k64, kState, 0); // x8 = GuestState*

    // Detect tight single-block loops (`steps[0..m-2]` followed by `Jcc` back to
    // `first.address`) where `steps[m-2]` directly drives hardware NZCV. When
    // `enable_self_loop` is set by `TranslationCache`, the entire loop executes
    // natively inside AArch64 registers for up to `max_runs` (`x1`) iterations
    // and materialises all architectural exit flags once on the cold exit path.
    a64::Cond self_loop_taken_cond = a64::Cond::kAl;
    const bool use_self_loop = enable_self_loop && steps.size() >= 2 && exit == BlockExit::kJcc &&
                               steps.back().insn.dst.imm == first.address &&
                               try_nzcv_cond(steps[steps.size() - 2].insn.opcode,
                                             steps.back().insn.condition, self_loop_taken_cond);

    if (use_self_loop) {
        constexpr a64::Reg kLoopA = 2;
        constexpr a64::Reg kLoopB = 3;
        constexpr a64::Reg kLoopRes = 4;
        constexpr a64::Reg kLoopRem = 7;

        // `x1` holds `max_runs` (>= 1) untouched across the loop; `x7` counts down.
        e.mov_reg(a64::Width::k64, kLoopRem, 1);
        emit_prologue(e, ra);

        const a64::Label loop_top = e.new_label();
        const a64::Label exit_fallthrough = e.new_label();
        const a64::Label emit_exit_flags = e.new_label();

        e.bind(loop_top);
        for (std::size_t i = 0; i + 2 < steps.size(); ++i) {
            const TranslateError emit_err =
                emit_one(e, steps[i].insn, /*write_rip=*/false, steps[i].live_flags, &ra);
            if (emit_err != TranslateError::kNone) {
                out.error = emit_err;
                return out;
            }
        }

        // Emit `steps[m - 2]` directly into its destination register `R` while
        // keeping pre-operation operand(s) in `kLoopA` (and `kLoopB` only when
        // non-immediate) so both cold exit paths produce bit-for-bit identical
        // CF/PF/AF/ZF/SF/OF in GuestState with minimal loop instructions.
        const Instruction& cmp_or_alu = steps[steps.size() - 2].insn;
        const bool is32 = cmp_or_alu.dst.size == OperandSize::k32;
        const a64::Width w = is32 ? a64::Width::k32 : a64::Width::k64;
        const int bits = is32 ? 32 : 64;
        const bool is_logical =
            cmp_or_alu.opcode == Opcode::kAnd || cmp_or_alu.opcode == Opcode::kOr ||
            cmp_or_alu.opcode == Opcode::kXor || cmp_or_alu.opcode == Opcode::kTest;
        const bool is_sub = cmp_or_alu.opcode == Opcode::kSub ||
                            cmp_or_alu.opcode == Opcode::kCmp || cmp_or_alu.opcode == Opcode::kDec;
        const bool is_inc_dec =
            cmp_or_alu.opcode == Opcode::kInc || cmp_or_alu.opcode == Opcode::kDec;
        const bool writes_dst =
            cmp_or_alu.opcode != Opcode::kCmp && cmp_or_alu.opcode != Opcode::kTest;
        const a64::Reg R =
            (writes_dst && ra.mapped(cmp_or_alu.dst.reg)) ? ra.host(cmp_or_alu.dst.reg) : kLoopRes;

        std::uint32_t loop_imm12 = 0;
        bool loop_shift12 = false;
        const std::uint64_t loop_imm_val =
            cmp_or_alu.has_src
                ? ((bits == 32) ? (cmp_or_alu.src.imm & 0xFFFFFFFFULL) : cmp_or_alu.src.imm)
                : 0ULL;
        const bool loop_imm_folded = !is_logical && !is_inc_dec && cmp_or_alu.has_src &&
                                     cmp_or_alu.src.kind == OperandKind::kImmediate &&
                                     encode_imm12(loop_imm_val, loop_imm12, loop_shift12);

        load_gpr_rb(e, &ra, cmp_or_alu.dst.reg, kLoopA);
        if (is_logical) {
            const TranslateError err = load_source(e, cmp_or_alu.src, kLoopB, bits, &ra);
            if (err != TranslateError::kNone) {
                out.error = err;
                return out;
            }
            switch (cmp_or_alu.opcode) {
            case Opcode::kAnd:
            case Opcode::kTest:
                e.and_(w, R, kLoopA, kLoopB);
                break;
            case Opcode::kOr:
                e.orr(w, R, kLoopA, kLoopB);
                break;
            default:
                e.eor(w, R, kLoopA, kLoopB);
                break;
            }
            e.cmp_imm(w, R, 0);
        } else if (is_inc_dec) {
            if (is_sub) {
                e.subs_imm(w, R, kLoopA, 1);
            } else {
                e.adds_imm(w, R, kLoopA, 1);
            }
        } else if (loop_imm_folded) {
            if (is_sub) {
                e.subs_imm(w, R, kLoopA, loop_imm12, loop_shift12);
            } else {
                e.adds_imm(w, R, kLoopA, loop_imm12, loop_shift12);
            }
        } else {
            const TranslateError err = load_source(e, cmp_or_alu.src, kLoopB, bits, &ra);
            if (err != TranslateError::kNone) {
                out.error = err;
                return out;
            }
            if (is_sub) {
                e.subs(w, R, kLoopA, kLoopB);
            } else {
                e.adds(w, R, kLoopA, kLoopB);
            }
        }
        if (writes_dst && R == kLoopRes) {
            store_gpr(e, kLoopRes, cmp_or_alu.dst.reg);
        }

        // Hot backedge: exit immediately if Jcc condition is false; otherwise
        // decrement `x7` and jump straight back to `loop_top`.
        e.b_cond(invert_cond(self_loop_taken_cond), exit_fallthrough);
        e.subs_imm(a64::Width::k64, kLoopRem, kLoopRem, 1);
        e.b_cond(a64::Cond::kNe, loop_top);

        // Cold exit A: step budget (`x1 == max_runs`) exhausted on taken backedge.
        if (is_logical) {
            e.cmp_imm(w, R, 0);
        } else if (is_inc_dec) {
            if (is_sub) {
                e.subs_imm(w, a64::kXzr, kLoopA, 1);
            } else {
                e.adds_imm(w, a64::kXzr, kLoopA, 1);
            }
        } else if (loop_imm_folded) {
            if (is_sub) {
                e.subs_imm(w, a64::kXzr, kLoopA, loop_imm12, loop_shift12);
            } else {
                e.adds_imm(w, a64::kXzr, kLoopA, loop_imm12, loop_shift12);
            }
        } else {
            if (is_sub) {
                e.subs(w, a64::kXzr, kLoopA, kLoopB);
            } else {
                e.adds(w, a64::kXzr, kLoopA, kLoopB);
            }
        }
        e.mov_imm64(kT0, steps.back().insn.dst.imm);
        e.b(emit_exit_flags);

        // Cold exit B: loop terminated naturally (`Jcc` not taken -> fallthrough).
        // Hardware NZCV is still untouched from `subs/adds R, ...` above!
        e.bind(exit_fallthrough);
        e.sub(a64::Width::k64, 1, 1, kLoopRem);
        e.add_imm(a64::Width::k64, 1, 1, 1); // x1 = runs = max_runs - rem + 1
        e.mov_imm64(kT0, steps.back().insn.address + steps.back().insn.length);

        // Shared cold epilogue: write `rip`, materialise exit flags into
        // GuestState, spill allocated GPRs, and return `(x0, x1)`.
        e.bind(emit_exit_flags);
        e.str(a64::Width::k64, kT0, kState, kRipOffset);
        const std::uint8_t exit_mask = flags_written(cmp_or_alu.opcode);
        const std::uint64_t af_imm = is_inc_dec ? 1ULL : loop_imm_val;
        const auto loop_eager_mask = static_cast<std::uint8_t>(exit_mask & ~kFbPfAf);
        if (is_logical) {
            emit_flags_logical(e, w, loop_eager_mask, R);
            emit_pf_af_store(e, exit_mask, R, a64::kXzr); // logical: AF is 0
        } else if (is_inc_dec || loop_imm_folded) {
            const bool af_from_xor = (exit_mask & kFbPfAf) != 0 && (af_imm & 0xFULL) != 0;
            if (af_from_xor) {
                e.eor(a64::Width::k64, kT2, kLoopA, R);
                if ((af_imm & 0x10ULL) != 0) {
                    e.mvn(a64::Width::k32, kT2, kT2);
                }
            }
            if (loop_eager_mask != 0) {
                emit_flags_from_nzcv(e, is_sub, loop_eager_mask, kT2, R);
            }
            emit_pf_af_store(e, exit_mask, R, af_from_xor ? kT2 : a64::kXzr);
        } else {
            const bool pfaf_live = (exit_mask & kFbPfAf) != 0;
            if (pfaf_live) {
                e.eor(a64::Width::k64, kT2, kLoopA, kLoopB);
                e.eor(a64::Width::k64, kT2, kT2, R);
            }
            if (loop_eager_mask != 0) {
                emit_flags_from_nzcv(e, is_sub, loop_eager_mask, kT2, R);
            }
            emit_pf_af_store(e, exit_mask, R, kT2);
        }
        emit_return(e, BlockResult::kOk, &ra, /*set_single_run=*/false);

        out.exit = exit;
        out.target = target;
        out.alternate = alternate;
        out.has_alternate = (exit == BlockExit::kJcc);
        out.guest_instruction_count = steps.size();
        out.code = e.bytes();
        out.instruction_count = e.count();
        return out;
    }

    // Standard single-execution block emission (with direct hardware NZCV fusion
    // on `ALU/CMP/INC/DEC -> Jcc`).
    emit_prologue(e, ra);

    for (std::size_t idx = 0; idx < steps.size(); ++idx) {
        const Step& step = steps[idx];
        const Opcode op = step.insn.opcode;
        if (op == Opcode::kHlt || op == Opcode::kRet) {
            emit_set_rip(e, step.insn.address);
            emit_return(e, BlockResult::kHalted, &ra, enable_self_loop);
            break;
        }
        if (op == Opcode::kSyscall) {
            emit_set_rip(e, step.insn.address + step.insn.length);
            emit_return(e, BlockResult::kSyscall, &ra, enable_self_loop);
            break;
        }
        if (op == Opcode::kJmp) {
            emit_set_rip(e, step.insn.dst.imm);
            emit_return(e, BlockResult::kOk, &ra, enable_self_loop);
            break;
        }
        if (op == Opcode::kJcc) {
            const std::uint64_t fallthrough = step.insn.address + step.insn.length;
            const a64::Label taken = e.new_label();
            const a64::Label done = e.new_label();
            a64::Cond fused = a64::Cond::kAl;
            if (idx > 0 && try_nzcv_cond(steps[idx - 1].insn.opcode, step.insn.condition, fused)) {
                e.b_cond(fused, taken);
            } else {
                emit_cond_branch(e, step.insn.condition, taken);
            }
            e.mov_imm64(kT0, fallthrough);
            e.b(done);
            e.bind(taken);
            e.mov_imm64(kT0, step.insn.dst.imm);
            e.bind(done);
            e.str(a64::Width::k64, kT0, kState, kRipOffset);
            emit_return(e, BlockResult::kOk, &ra, enable_self_loop);
            break;
        }
        const TranslateError emit_err =
            emit_one(e, step.insn, /*write_rip=*/false, step.live_flags, &ra);
        if (emit_err != TranslateError::kNone) {
            out.error = emit_err;
            return out;
        }
    }

    if (exit == BlockExit::kFallThrough) {
        emit_set_rip(e, target);
        emit_return(e, BlockResult::kOk, &ra, enable_self_loop);
    }

    out.exit = exit;
    out.target = target;
    out.alternate = alternate;
    out.has_alternate = (exit == BlockExit::kJcc);
    out.guest_instruction_count = steps.size();
    out.code = e.bytes();
    out.instruction_count = e.count();
    return out;
}

// ============================================================================
// Full-System Tier 1 Cached Block Compiler + Shared Trampolines
// ============================================================================

namespace {

constexpr a64::Reg kDispatchReg = 24;      // void* jit_dispatch (callee-saved)
constexpr a64::Reg kInitBudget = 25;       // initial step budget (callee-saved)
constexpr a64::Reg kRemBudget = 26;        // remaining step budget (callee-saved)
constexpr a64::Reg kSavedState = 27;       // GuestState* (callee-saved mirror of x8)
constexpr a64::Reg kCtxReg = 28;           // ExecContext* (callee-saved)
constexpr std::size_t kCachedMaxAlloc = 5; // x19..x23

constexpr unsigned kFsBaseOffset = offsetof(GuestState, fs_base);
static_assert(kFsBaseOffset % 8 == 0 && kFsBaseOffset < 4096);

constexpr unsigned kScratchSlot0 = 96;  // [sp + 96]: 8B scratch for slow SoftTlb C call
constexpr unsigned kScratchSlot1 = 104; // [sp + 104]: 8B saved EA for RMW memory instructions
constexpr unsigned kScratchSlot2 = 112; // [sp + 112]: 16B-aligned SSE2 memory scratch (lo)
constexpr unsigned kScratchSlot3 = 120; // [sp + 120]: 16B-aligned SSE2 memory scratch (hi)

static_assert((offsetof(GuestState, xmm) % 16) == 0);
static_assert(offsetof(GuestState, xmm) + (kXmmCount - 1) * sizeof(XmmReg) <= 504);

constexpr unsigned xmm_offset(std::uint8_t idx) {
    return static_cast<unsigned>(offsetof(GuestState, xmm) +
                                 static_cast<std::size_t>(idx) * sizeof(XmmReg));
}

void emit_ldr_q(a64::Encoder& e, unsigned qt, a64::Reg rn, unsigned byte_off) {
    e.emit_raw_u32(0x3DC00000u | ((byte_off / 16u) << 10) | (static_cast<std::uint32_t>(rn) << 5) |
                   (qt & 31u));
}

void emit_str_q(a64::Encoder& e, unsigned qt, a64::Reg rn, unsigned byte_off) {
    e.emit_raw_u32(0x3D800000u | ((byte_off / 16u) << 10) | (static_cast<std::uint32_t>(rn) << 5) |
                   (qt & 31u));
}

struct SlowReadStub {
    a64::Label slow_lbl;
    a64::Label done_lbl;
    a64::Label fault_lbl;
    a64::Reg addr_reg;
    a64::Reg out_reg;
    unsigned bytes;
};

struct SlowWriteStub {
    a64::Label slow_lbl;
    a64::Label done_lbl;
    a64::Label fault_lbl;
    a64::Reg addr_reg;
    a64::Reg val_reg;
    unsigned bytes;
};

struct FaultStub {
    a64::Label fault_lbl;
    std::uint64_t fault_pc;
    std::uint32_t unretired_insns;
    bool restore_rsp = false;
};

struct EmbeddedInsn {
    a64::Label label;
    Instruction insn;
};

int op_bits(OperandSize sz) {
    switch (sz) {
    case OperandSize::k8:
        return 8;
    case OperandSize::k16:
        return 16;
    case OperandSize::k32:
        return 32;
    case OperandSize::k64:
        return 64;
    case OperandSize::k128:
        return 128;
    }
    return 64;
}

unsigned op_bytes(OperandSize sz) {
    return static_cast<unsigned>(op_bits(sz) / 8);
}

// Conservative flag liveness for the full baseline x86-64 + SSE2 ISA:
// - `flags_killed_full`: flags *unconditionally* overwritten on every dynamic path.
//   Variable shifts (`cl == 0`) and `REP` string ops (`rcx == 0`) may leave prior
//   flags intact, so their `killed` mask is 0 while their `written` mask marks
//   what they *may* write when live!
std::uint8_t flags_read_full(const Instruction& in) {
    switch (in.opcode) {
    case Opcode::kJcc:
    case Opcode::kSetcc:
    case Opcode::kCmovcc:
        return condition_flags(in.condition);
    case Opcode::kAdc:
    case Opcode::kSbb:
        return kFbCf;
    case Opcode::kShl:
    case Opcode::kShr:
    case Opcode::kSar:
    case Opcode::kRol:
    case Opcode::kRor:
    case Opcode::kShld:
    case Opcode::kShrd:
        // If dynamic count == 0 at runtime, flags in GuestState pass through!
        if (in.opcode == Opcode::kShld || in.opcode == Opcode::kShrd) {
            return (in.aux.kind == OperandKind::kImmediate && (in.aux.imm & 31ULL) != 0) ? 0
                                                                                         : kFbAll;
        }
        return (in.src.kind == OperandKind::kImmediate && (in.src.imm & 31ULL) != 0) ? 0 : kFbAll;
    case Opcode::kStringOp:
        return kFbAll;
    default:
        return 0;
    }
}

std::uint8_t flags_killed_full(const Instruction& in) {
    switch (in.opcode) {
    case Opcode::kAdd:
    case Opcode::kAdc:
    case Opcode::kSub:
    case Opcode::kSbb:
    case Opcode::kCmp:
    case Opcode::kNeg:
    case Opcode::kTest:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
    case Opcode::kCmpxchg:
    case Opcode::kXadd:
        return kFbAll;
    case Opcode::kInc:
    case Opcode::kDec:
        return static_cast<std::uint8_t>(kFbAll & ~kFbCf);
    case Opcode::kMul:
    case Opcode::kImul:
        return static_cast<std::uint8_t>(kFbCf | kFbOf);
    case Opcode::kBt:
    case Opcode::kBts:
    case Opcode::kBtr:
    case Opcode::kBtc:
        return kFbCf;
    case Opcode::kBsf:
    case Opcode::kBsr:
        return (in.rep == 0xF3) ? static_cast<std::uint8_t>(kFbZf | kFbCf)
                                : static_cast<std::uint8_t>(kFbZf);
    default:
        return 0;
    }
}

std::uint8_t flags_written_full(const Instruction& in) {
    const std::uint8_t killed = flags_killed_full(in);
    if (killed != 0) {
        return killed;
    }
    switch (in.opcode) {
    case Opcode::kShl:
    case Opcode::kShr:
    case Opcode::kSar:
    case Opcode::kRol:
    case Opcode::kRor:
    case Opcode::kShld:
    case Opcode::kShrd:
    case Opcode::kStringOp:
        return kFbAll;
    default:
        return 0;
    }
}

void mark_mem_regs(const MemoryOperand& m, bool reads[kGprCount]) {
    if (!m.rip_relative && m.has_base) {
        reads[static_cast<unsigned>(m.base)] = true;
    }
    if (m.has_index) {
        reads[static_cast<unsigned>(m.index)] = true;
    }
}

void regs_read_write_full(const Instruction& insn, bool reads[kGprCount], bool writes[kGprCount]) {
    auto note_op = [&](const Operand& op, bool is_read, bool is_write) {
        if (op.kind == OperandKind::kRegister) {
            if (is_read) {
                reads[static_cast<unsigned>(op.reg)] = true;
            }
            if (is_write) {
                writes[static_cast<unsigned>(op.reg)] = true;
            }
        } else if (op.kind == OperandKind::kMemory) {
            mark_mem_regs(op.mem, reads);
        }
    };

    switch (insn.opcode) {
    case Opcode::kMov:
    case Opcode::kMovzx:
    case Opcode::kMovsx:
        note_op(insn.src, true, false);
        note_op(insn.dst, false, true);
        break;
    case Opcode::kLea:
        mark_mem_regs(insn.src.mem, reads);
        note_op(insn.dst, false, true);
        break;
    case Opcode::kAdd:
    case Opcode::kAdc:
    case Opcode::kSub:
    case Opcode::kSbb:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
    case Opcode::kShl:
    case Opcode::kShr:
    case Opcode::kSar:
    case Opcode::kRol:
    case Opcode::kRor:
        note_op(insn.dst, true, true);
        note_op(insn.src, true, false);
        break;
    case Opcode::kCmp:
    case Opcode::kTest:
        note_op(insn.dst, true, false);
        note_op(insn.src, true, false);
        break;
    case Opcode::kInc:
    case Opcode::kDec:
    case Opcode::kNot:
    case Opcode::kNeg:
    case Opcode::kBswap:
        note_op(insn.dst, true, true);
        break;
    case Opcode::kImul:
        if (!insn.has_src) {
            note_op(insn.dst, true, false);
            reads[static_cast<unsigned>(Gpr::kRax)] = true;
            writes[static_cast<unsigned>(Gpr::kRax)] = true;
            writes[static_cast<unsigned>(Gpr::kRdx)] = true;
        } else if (insn.has_aux) {
            note_op(insn.src, true, false);
            note_op(insn.aux, true, false);
            note_op(insn.dst, false, true);
        } else {
            note_op(insn.dst, true, true);
            note_op(insn.src, true, false);
        }
        break;
    case Opcode::kSetcc:
        note_op(insn.dst, false, true);
        break;
    case Opcode::kCmovcc:
    case Opcode::kBsf:
    case Opcode::kBsr:
        note_op(insn.src, true, false);
        note_op(insn.dst, true, true);
        break;
    case Opcode::kPush:
        note_op(insn.dst, true, false);
        reads[static_cast<unsigned>(Gpr::kRsp)] = true;
        writes[static_cast<unsigned>(Gpr::kRsp)] = true;
        break;
    case Opcode::kPop:
        note_op(insn.dst, false, true);
        reads[static_cast<unsigned>(Gpr::kRsp)] = true;
        writes[static_cast<unsigned>(Gpr::kRsp)] = true;
        break;
    case Opcode::kCall:
        note_op(insn.dst, true, false);
        reads[static_cast<unsigned>(Gpr::kRsp)] = true;
        writes[static_cast<unsigned>(Gpr::kRsp)] = true;
        break;
    case Opcode::kRet:
        reads[static_cast<unsigned>(Gpr::kRsp)] = true;
        writes[static_cast<unsigned>(Gpr::kRsp)] = true;
        break;
    case Opcode::kLeave:
        reads[static_cast<unsigned>(Gpr::kRbp)] = true;
        writes[static_cast<unsigned>(Gpr::kRbp)] = true;
        writes[static_cast<unsigned>(Gpr::kRsp)] = true;
        break;
    case Opcode::kJmp:
        note_op(insn.dst, true, false);
        break;
    case Opcode::kSse:
        note_op(insn.dst, false, true);
        if (insn.has_src) {
            note_op(insn.src, true, false);
        }
        if (insn.sse_op == SseOp::kXgetbv) {
            reads[static_cast<unsigned>(Gpr::kRcx)] = true;
            writes[static_cast<unsigned>(Gpr::kRax)] = true;
            writes[static_cast<unsigned>(Gpr::kRdx)] = true;
        } else if (insn.sse_op == SseOp::kX87 && insn.aux.imm == 0xDFE0u) {
            reads[static_cast<unsigned>(Gpr::kRax)] = true;
            writes[static_cast<unsigned>(Gpr::kRax)] = true;
        }
        break;
    default:
        break;
    }
}

namespace {

// Does this operand form touch guest memory?
bool operand_is_memory(const Operand& op) {
    return op.kind == OperandKind::kMemory;
}

} // namespace

// The facts themselves. In this anonymous namespace so they see the flag and
// register helpers above directly; the exported `hyperion::compute_ir_facts`
// wrapper at the end of the file is what other translation units call.
IrFacts compute_facts(const Instruction& insn) {
    IrFacts f;

    bool r[kGprCount] = {};
    bool w[kGprCount] = {};
    regs_read_write_full(insn, r, w);
    for (unsigned i = 0; i < kGprCount; ++i) {
        const auto bit = static_cast<std::uint16_t>(1u << i);
        if (r[i]) {
            f.gpr_reads = static_cast<std::uint16_t>(f.gpr_reads | bit);
        }
        if (w[i]) {
            f.gpr_writes = static_cast<std::uint16_t>(f.gpr_writes | bit);
        }
    }

    f.flags_read = flags_read_full(insn);
    f.flags_killed = flags_killed_full(insn);
    f.flags_written = flags_written_full(insn);

    // Memory effects. Deliberately conservative: any memory operand counts as a
    // read (an addressing form can always fault on load), and the destination
    // counts as a write only for opcodes that really store. Where the truth is
    // subtle (string operations move in the direction of DF, and the implicit
    // stack accesses of push/pop/call/ret/leave) both bits are set, so a future
    // pass that consults these can never elide or reorder across them.
    f.reads_memory = operand_is_memory(insn.dst) || (insn.has_src && operand_is_memory(insn.src));
    const bool dst_is_written = insn.opcode != Opcode::kCmp && insn.opcode != Opcode::kTest &&
                                insn.opcode != Opcode::kPush && insn.opcode != Opcode::kJmp &&
                                insn.opcode != Opcode::kJcc && insn.opcode != Opcode::kCall;
    f.writes_memory = dst_is_written && operand_is_memory(insn.dst);
    switch (insn.opcode) {
    case Opcode::kPush:
    case Opcode::kCall:
        f.writes_memory = true; // return address / operand onto the guest stack
        break;
    case Opcode::kPop:
    case Opcode::kRet:
        f.reads_memory = true; // pop the guest stack
        break;
    case Opcode::kLeave:
        f.reads_memory = true;
        f.writes_memory = true;
        break;
    case Opcode::kStringOp:
        f.reads_memory = true;
        f.writes_memory = true; // MOVS/STOS store; CMPS/SCAS only read
        break;
    default:
        break;
    }

    switch (insn.opcode) {
    case Opcode::kJmp:
    case Opcode::kJcc:
    case Opcode::kCall:
    case Opcode::kRet:
    case Opcode::kHlt:
    case Opcode::kSyscall:
        f.is_terminator = true;
        break;
    default:
        break;
    }

    f.step_weight = 1;
    return f;
}

void read_gpr_sized_jit(a64::Encoder& e, const RegAlloc& ra, Gpr r, OperandSize sz, bool high_byte,
                        a64::Reg dst) {
    load_gpr_rb(e, &ra, r, dst);
    switch (sz) {
    case OperandSize::k8:
        e.ubfx(a64::Width::k64, dst, dst, high_byte ? 8u : 0u, 8u);
        return;
    case OperandSize::k16:
        e.ubfx(a64::Width::k64, dst, dst, 0u, 16u);
        return;
    case OperandSize::k32:
        e.mov_reg(a64::Width::k32, dst, dst);
        return;
    case OperandSize::k64:
    case OperandSize::k128:
        return;
    }
}

void write_gpr_sized_jit(a64::Encoder& e, const RegAlloc& ra, Gpr r, OperandSize sz, bool high_byte,
                         a64::Reg val_reg, a64::Reg scratch) {
    if (ra.mapped(r)) {
        const a64::Reg h = ra.host(r);
        switch (sz) {
        case OperandSize::k8:
            e.bfi(a64::Width::k64, h, val_reg, high_byte ? 8u : 0u, 8u);
            return;
        case OperandSize::k16:
            e.bfi(a64::Width::k64, h, val_reg, 0u, 16u);
            return;
        case OperandSize::k32:
            e.mov_reg(a64::Width::k32, h, val_reg);
            return;
        case OperandSize::k64:
        case OperandSize::k128:
            if (h != val_reg) {
                e.mov_reg(a64::Width::k64, h, val_reg);
            }
            return;
        }
    }
    switch (sz) {
    case OperandSize::k8:
        e.strb(val_reg, kState, gpr_offset(r) + (high_byte ? 1u : 0u));
        return;
    case OperandSize::k16:
        e.strh(val_reg, kState, gpr_offset(r));
        return;
    case OperandSize::k32:
        e.mov_reg(a64::Width::k32, scratch, val_reg);
        e.str(a64::Width::k64, scratch, kState, gpr_offset(r));
        return;
    case OperandSize::k64:
    case OperandSize::k128:
        e.str(a64::Width::k64, val_reg, kState, gpr_offset(r));
        return;
    }
}

void emit_effective_address_jit(a64::Encoder& e, const RegAlloc& ra, const MemoryOperand& mem,
                                std::uint64_t next_rip, a64::Reg dst, a64::Reg tmp) {
    bool have_val = false;
    if (mem.rip_relative) {
        e.mov_imm64(dst, next_rip + static_cast<std::uint64_t>(mem.disp));
    } else {
        if (mem.has_base) {
            load_gpr_rb(e, &ra, mem.base, dst);
            have_val = true;
        }
        if (mem.has_index) {
            load_gpr_rb(e, &ra, mem.index, tmp);
            const unsigned shift =
                (mem.scale == 8) ? 3u : ((mem.scale == 4) ? 2u : ((mem.scale == 2) ? 1u : 0u));
            if (!have_val) {
                if (shift == 0) {
                    e.mov_reg(a64::Width::k64, dst, tmp);
                } else {
                    e.lsl_imm(a64::Width::k64, dst, tmp, shift);
                }
                have_val = true;
            } else if (shift == 0) {
                e.add(a64::Width::k64, dst, dst, tmp);
            } else {
                e.add_shifted(a64::Width::k64, dst, dst, tmp, a64::Shift::kLsl, shift);
            }
        }
        if (mem.disp != 0 || !have_val) {
            const auto udisp = static_cast<std::uint64_t>(mem.disp);
            if (!have_val) {
                e.mov_imm64(dst, udisp);
            } else {
                std::uint32_t imm12 = 0;
                bool sh12 = false;
                if (mem.disp > 0 && encode_imm12(udisp, imm12, sh12)) {
                    e.add_imm(a64::Width::k64, dst, dst, imm12, sh12);
                } else if (mem.disp < 0 && encode_imm12(0ULL - udisp, imm12, sh12)) {
                    e.sub_imm(a64::Width::k64, dst, dst, imm12, sh12);
                } else {
                    e.mov_imm64(tmp, udisp);
                    e.add(a64::Width::k64, dst, dst, tmp);
                }
            }
        }
    }
    if (mem.address_size == OperandSize::k32) {
        e.mov_reg(a64::Width::k32, dst, dst);
    }
    if (mem.fs_relative) {
        e.ldr(a64::Width::k64, tmp, kState, kFsBaseOffset);
        e.add(a64::Width::k64, dst, dst, tmp);
    }
}

void emit_inline_tlb_read(a64::Encoder& e, a64::Reg addr_reg, a64::Reg out_reg, unsigned bytes,
                          a64::Label fault_lbl, std::vector<SlowReadStub>& stubs) {
    // Scratch used: kT0(12), kT1(13), kT2(14), kT3(15). Does NOT touch `addr_reg` until final load.
    const a64::Label slow_lbl = e.new_label();
    const a64::Label done_lbl = e.new_label();
    e.ubfx(a64::Width::k64, kT0, addr_reg, 12, kSoftTlbBits);      // slot = (addr >> 12) & 511
    e.add_imm(a64::Width::k64, kT1, kCtxReg, 1, /*shift12=*/true); // &ctx->tlb.read_entries[0]
    e.add_shifted(a64::Width::k64, kT1, kT1, kT0, a64::Shift::kLsl, 4);
    e.ldp_off(kT2, kT3, kT1, 0);                // kT2=tag, kT3=host_addend
    e.sub(a64::Width::k64, kT2, addr_reg, kT2); // page_off = addr - tag
    e.cmp_imm(a64::Width::k64, kT2, 4096u - bytes);
    e.b_cond(a64::Cond::kHi, slow_lbl);
    e.ldr_reg_sized(bytes, out_reg, addr_reg, kT3);
    e.bind(done_lbl);
    stubs.push_back({slow_lbl, done_lbl, fault_lbl, addr_reg, out_reg, bytes});
}

void emit_inline_tlb_write(a64::Encoder& e, a64::Reg addr_reg, a64::Reg val_reg, unsigned bytes,
                           a64::Label fault_lbl, std::vector<SlowWriteStub>& stubs) {
    const a64::Label slow_lbl = e.new_label();
    const a64::Label done_lbl = e.new_label();
    e.ubfx(a64::Width::k64, kT0, addr_reg, 12, kSoftTlbBits);
    e.add_imm(a64::Width::k64, kT1, kCtxReg, 3, /*shift12=*/true); // &ctx->tlb.write_entries[0]
    e.add_shifted(a64::Width::k64, kT1, kT1, kT0, a64::Shift::kLsl, 4);
    e.ldp_off(kT2, kT3, kT1, 0);
    e.sub(a64::Width::k64, kT2, addr_reg, kT2);
    e.cmp_imm(a64::Width::k64, kT2, 4096u - bytes);
    e.b_cond(a64::Cond::kHi, slow_lbl);
    e.str_reg_sized(bytes, val_reg, addr_reg, kT3);
    e.bind(done_lbl);
    stubs.push_back({slow_lbl, done_lbl, fault_lbl, addr_reg, val_reg, bytes});
}

// Materialise x86 condition `c` into 0/1 in `dst` using `GuestState::flags`.
void emit_eval_cond_to_reg(a64::Encoder& e, Condition c, a64::Reg dst) {
    switch (c) {
    case Condition::kO:
        e.ldrb(dst, kState, kOfOffset);
        return;
    case Condition::kNo:
        e.ldrb(dst, kState, kOfOffset);
        e.mvn(a64::Width::k32, dst, dst);
        e.ubfx(a64::Width::k32, dst, dst, 0, 1);
        return;
    case Condition::kB:
        e.ldrb(dst, kState, kCfOffset);
        return;
    case Condition::kAe:
        e.ldrb(dst, kState, kCfOffset);
        e.mvn(a64::Width::k32, dst, dst);
        e.ubfx(a64::Width::k32, dst, dst, 0, 1);
        return;
    case Condition::kE:
        e.ldrb(dst, kState, kZfOffset);
        return;
    case Condition::kNe:
        e.ldrb(dst, kState, kZfOffset);
        e.mvn(a64::Width::k32, dst, dst);
        e.ubfx(a64::Width::k32, dst, dst, 0, 1);
        return;
    case Condition::kS:
        e.ldrb(dst, kState, kSfOffset);
        return;
    case Condition::kNs:
        e.ldrb(dst, kState, kSfOffset);
        e.mvn(a64::Width::k32, dst, dst);
        e.ubfx(a64::Width::k32, dst, dst, 0, 1);
        return;
    case Condition::kP:
        emit_pf_af_guard(e, kFbPf); // see emit_cond_branch: PF may be pending
        e.ldrb(dst, kState, kPfOffset);
        return;
    case Condition::kNp:
        emit_pf_af_guard(e, kFbPf);
        e.ldrb(dst, kState, kPfOffset);
        e.mvn(a64::Width::k32, dst, dst);
        e.ubfx(a64::Width::k32, dst, dst, 0, 1);
        return;
    case Condition::kBe:
    case Condition::kA:
        e.ldrb(dst, kState, kCfOffset);
        e.ldrb(kT1, kState, kZfOffset);
        e.orr(a64::Width::k32, dst, dst, kT1);
        if (c == Condition::kA) {
            e.mvn(a64::Width::k32, dst, dst);
            e.ubfx(a64::Width::k32, dst, dst, 0, 1);
        }
        return;
    case Condition::kL:
    case Condition::kGe:
        e.ldrb(dst, kState, kSfOffset);
        e.ldrb(kT1, kState, kOfOffset);
        e.eor(a64::Width::k32, dst, dst, kT1);
        if (c == Condition::kGe) {
            e.mvn(a64::Width::k32, dst, dst);
            e.ubfx(a64::Width::k32, dst, dst, 0, 1);
        }
        return;
    case Condition::kLe:
    case Condition::kG:
        e.ldrb(dst, kState, kSfOffset);
        e.ldrb(kT1, kState, kOfOffset);
        e.ldrb(kT2, kState, kZfOffset);
        e.eor(a64::Width::k32, dst, dst, kT1);
        e.orr(a64::Width::k32, dst, dst, kT2);
        if (c == Condition::kG) {
            e.mvn(a64::Width::k32, dst, dst);
            e.ubfx(a64::Width::k32, dst, dst, 0, 1);
        }
        return;
    }
}

bool is_classic_fast_alu(const Instruction& insn) {
    if (insn.dst.kind != OperandKind::kRegister || insn.dst.high_byte) {
        return false;
    }
    if (insn.dst.size != OperandSize::k32 && insn.dst.size != OperandSize::k64) {
        return false;
    }
    if (insn.has_src) {
        if (insn.src.kind == OperandKind::kRegister && insn.src.high_byte) {
            return false;
        }
        if (insn.src.kind != OperandKind::kRegister && insn.src.kind != OperandKind::kImmediate) {
            if (insn.opcode != Opcode::kLea) {
                return false;
            }
        }
    }
    switch (insn.opcode) {
    case Opcode::kMov:
    case Opcode::kLea:
    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kCmp:
    case Opcode::kTest:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
    case Opcode::kInc:
    case Opcode::kDec:
    case Opcode::kNop:
        return true;
    default:
        return false;
    }
}

} // namespace

// Exported wrapper: tests and other translation units ask for the facts without
// seeing the flag/register helper predicates they are derived from.
IrFacts compute_ir_facts(const Instruction& insn) {
    return compute_facts(insn);
}

JitTrampolines build_jit_trampolines() {
    JitTrampolines out;

    // 1. Entry trampoline:
    //    `RawBlockReturn enter_jit(GuestState* x0, uint64_t budget x1,
    //                              ExecContext* x2, void* block x3, void* dispatch x4)`
    {
        a64::Encoder e;
        e.stp_pre(29, 30, a64::kSp, -128);
        e.stp_off(19, 20, a64::kSp, 16);
        e.stp_off(21, 22, a64::kSp, 32);
        e.stp_off(23, 24, a64::kSp, 48);
        e.stp_off(25, 26, a64::kSp, 64);
        e.stp_off(27, 28, a64::kSp, 80);
        e.mov_reg(a64::Width::k64, kState, 0);       // x8 = GuestState*
        e.mov_reg(a64::Width::k64, kSavedState, 0);  // x27 = GuestState*
        e.mov_reg(a64::Width::k64, kInitBudget, 1);  // x25 = initial budget
        e.mov_reg(a64::Width::k64, kRemBudget, 1);   // x26 = remaining budget
        e.mov_reg(a64::Width::k64, kCtxReg, 2);      // x28 = ExecContext*
        e.mov_reg(a64::Width::k64, kDispatchReg, 4); // x24 = jit_dispatch
        e.ldr(a64::Width::k64, 29, 2, offsetof(ExecContext, jit_budget_epilogue));
        e.br(3); // jump into first block!
        out.entry_code = e.bytes();
    }

    // 2. Shared in-JIT jump-table dispatcher: expects `x9` (`kA`) = next guest `rip`.
    {
        a64::Encoder e;
        const a64::Label miss = e.new_label();
        e.str(a64::Width::k64, kA, kState, kRipOffset);
        e.lsr_imm(a64::Width::k64, kB, kA, 12);
        e.eor(a64::Width::k64, kB, kB, kA);
        e.ubfx(a64::Width::k64, kB, kB, 0, kDirectJumpBits);
        e.add_imm(a64::Width::k64, kT1, kCtxReg, 6, /*shift12=*/true);
        e.add_shifted(a64::Width::k64, kT1, kT1, kB, a64::Shift::kLsl, 4);
        e.ldp_off(kT2, kT3, kT1, 0);
        e.cmp(a64::Width::k64, kT2, kA);
        e.b_cond(a64::Cond::kNe, miss);
        e.br(kT3);

        e.bind(miss);
        e.movz(a64::Width::k64, 0, 0); // x0 = BlockResult::kOk
        e.ldr(a64::Width::k64, kA, kCtxReg, offsetof(ExecContext, jit_exit_epilogue));
        e.br(kA);
        out.dispatch_code = e.bytes();
    }

    // 2b. Shared budget-exhaustion epilogue (`x29` inside JIT = `ctx->jit_budget_epilogue`).
    {
        a64::Encoder e;
        e.movz(a64::Width::k64, 0, 0); // x0 = BlockResult::kOk
        e.ldr(a64::Width::k64, kA, kCtxReg, offsetof(ExecContext, jit_exit_epilogue));
        e.br(kA);
        out.budget_code = e.bytes();
    }

    // 3. Shared exit epilogue (`ctx->jit_exit_epilogue`): expects `x0` = `BlockResult`.
    {
        a64::Encoder e;
        // Materialise any pending PF/AF *before* control reaches C++: this is the
        // one funnel every block exit, budget exhaustion, dispatcher miss and
        // fault path goes through, so the interpreter, syscall layer and tests can
        // never observe a stale `GuestState::flags`. Costs two instructions when
        // nothing is pending. Deliberately not emitted on the dispatcher's
        // jump-table hit path, which stays inside generated code: a linked block
        // that does not read PF/AF keeps the descriptor alive.
        emit_pf_af_guard(e, kFbPfAf, kSavedState);
        e.sub(a64::Width::k64, 1, kInitBudget, kRemBudget); // x1 = retired guest insns
        e.ldp_off(19, 20, a64::kSp, 16);
        e.ldp_off(21, 22, a64::kSp, 32);
        e.ldp_off(23, 24, a64::kSp, 48);
        e.ldp_off(25, 26, a64::kSp, 64);
        e.ldp_off(27, 28, a64::kSp, 80);
        e.ldp_post(29, 30, a64::kSp, 128);
        e.ret();
        out.exit_code = e.bytes();
    }

    return out;
}

// A cached block has at most a handful of exit sites (a terminator ends it), but
// a block of straight-line code can also fall out of the fetch window, so keep a
// small fixed cap rather than letting a pathological input grow the vector.
constexpr std::size_t kMaxExitLinks = 8;

BlockTranslation translate_cached_block(const std::uint8_t* code, std::size_t size,
                                        const Instruction& first) {
    BlockTranslation out;
    const PfAfDeferScope pf_af_scope(out);
    constexpr std::size_t kMaxCachedBlockInsns = 256;

    // P14 stage 1: the block is a vector of IrInsn (instruction + facts), so the
    // allocation and liveness passes below read facts instead of re-deriving
    // them, and later passes have somewhere to look them up.
    std::vector<IrInsn> steps;
    steps.reserve(16);

    Instruction insn = first;
    std::size_t next_offset = first.length;
    BlockExit exit = BlockExit::kFallThrough;
    std::uint64_t target = 0;
    std::uint64_t alternate = 0; // kJcc branch destination, for successor prefetch

    while (true) {
        const Opcode op = insn.opcode;
        if (op == Opcode::kHlt) {
            steps.push_back(IrInsn{insn, compute_ir_facts(insn), 0});
            exit = BlockExit::kHalted;
            break;
        }
        if (op == Opcode::kSyscall) {
            steps.push_back(IrInsn{insn, compute_ir_facts(insn), 0});
            exit = BlockExit::kSyscall;
            target = insn.address + insn.length;
            break;
        }
        if (op == Opcode::kJmp || op == Opcode::kCall || op == Opcode::kRet) {
            steps.push_back(IrInsn{insn, compute_ir_facts(insn), 0});
            exit = BlockExit::kJmp;
            target = (insn.dst.kind == OperandKind::kImmediate && op != Opcode::kRet) ? insn.dst.imm
                                                                                      : 0ULL;
            break;
        }
        if (op == Opcode::kJcc) {
            steps.push_back(IrInsn{insn, compute_ir_facts(insn), 0});
            exit = BlockExit::kJcc;
            target = insn.address + insn.length;
            alternate = insn.dst.imm;
            break;
        }

        steps.push_back(IrInsn{insn, compute_ir_facts(insn), 0});
        if (steps.size() >= kMaxCachedBlockInsns || next_offset >= size) {
            exit = BlockExit::kFallThrough;
            target = first.address + next_offset;
            break;
        }
        const DecodeResult next =
            decode(code + next_offset, size - next_offset, first.address + next_offset);
        if (!next.ok()) {
            exit = BlockExit::kFallThrough;
            target = first.address + next_offset;
            break;
        }
        insn = next.instruction;
        next_offset += insn.length;
    }

    // Pass 1.5: per-block register allocation into callee-saved `x19..x23`.
    // Because `enter_jit` already saved `x19..x23` once on stack frame entry,
    // allocating a guest register inside a cached block costs ONLY `2` words
    // (`1 ldr` at block entry + `1 str` at block exit), never `stp`/`ldp`!
    std::size_t refs[kGprCount] = {};
    for (const IrInsn& step : steps) {
        for (unsigned i = 0; i < kGprCount; ++i) {
            const auto bit = static_cast<std::uint16_t>(1u << i);
            refs[i] += static_cast<std::size_t>(((step.facts.gpr_reads & bit) != 0 ? 1u : 0u) +
                                                ((step.facts.gpr_writes & bit) != 0 ? 1u : 0u));
        }
    }

    RegAlloc ra;
    std::vector<unsigned> order;
    for (unsigned i = 0; i < kGprCount; ++i) {
        if (refs[i] > 2) { // strictly saves >= 1 word over 1 load + 1 store!
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
        return refs[a] != refs[b] ? refs[a] > refs[b] : a < b;
    });
    const std::size_t nalloc = std::min(order.size(), kCachedMaxAlloc);
    for (std::size_t i = 0; i < nalloc; ++i) {
        const auto h = static_cast<a64::Reg>(kAllocBase + i);
        ra.host_of_guest[order[i]] = static_cast<std::int8_t>(h);
        ra.allocated.push_back(h);
    }

    // Pass 2: exact backward flag liveness across the full ISA.
    std::uint8_t live_out = kFbAll;
    for (std::size_t i = steps.size(); i-- > 0;) {
        const IrFacts& facts = steps[i].facts;
        steps[i].live_flags = static_cast<std::uint8_t>(facts.flags_written & live_out);
        live_out = static_cast<std::uint8_t>(facts.flags_read | (live_out & ~facts.flags_killed));
    }

    a64::Encoder e;
    std::vector<SlowReadStub> read_stubs;
    std::vector<SlowWriteStub> write_stubs;
    std::vector<FaultStub> fault_stubs;
    std::vector<EmbeddedInsn> embedded_insns;

    const auto block_insns = static_cast<std::uint32_t>(steps.size());
    const a64::Label entry_budget_undo = e.new_label();

    auto spill_ra = [&]() {
        for (a64::Reg h : ra.allocated) {
            store_gpr(e, h, guest_of_host(ra, h));
        }
    };
    auto reload_ra = [&]() {
        for (a64::Reg h : ra.allocated) {
            load_gpr(e, guest_of_host(ra, h), h);
        }
    };
    auto make_fault_label = [&](std::size_t step_idx, bool restore_rsp = false) {
        const a64::Label lbl = e.new_label();
        fault_stubs.push_back({lbl, steps[step_idx].insn.address,
                               static_cast<std::uint32_t>(steps.size() - step_idx), restore_rsp});
        return lbl;
    };

    // Emit one non-control-flow instruction `steps[idx]`.
    auto emit_cached_body_insn = [&](std::size_t idx) -> TranslateError {
        const Instruction& in = steps[idx].insn;
        const std::uint8_t lflags = steps[idx].live_flags;
        const std::uint64_t next_rip = in.address + in.length;

        // Fast reuse of proven 32/64-bit register/imm ALU/LEA/MOV codegen!
        if (is_classic_fast_alu(in)) {
            return emit_one(e, in, /*write_rip=*/false, lflags, &ra);
        }

        const int bits = op_bits(in.dst.size);
        const unsigned bytes = op_bytes(in.dst.size);
        const a64::Width w = (bits == 64) ? a64::Width::k64 : a64::Width::k32;

        auto load_any_op = [&](const Operand& op, a64::Reg out_reg, a64::Label flbl) {
            if (op.kind == OperandKind::kRegister) {
                read_gpr_sized_jit(e, ra, op.reg, op.size, op.high_byte, out_reg);
            } else if (op.kind == OperandKind::kImmediate) {
                const std::uint64_t v =
                    (op.size == OperandSize::k32) ? (op.imm & 0xFFFFFFFFULL) : op.imm;
                e.mov_imm64(out_reg, v);
            } else {
                emit_effective_address_jit(e, ra, op.mem, next_rip, kB, kT0);
                emit_inline_tlb_read(e, kB, out_reg, op_bytes(op.size), flbl, read_stubs);
            }
        };

        switch (in.opcode) {
        case Opcode::kNop:
            return TranslateError::kNone;

        case Opcode::kMov:
        case Opcode::kMovzx: {
            const a64::Label flbl =
                (in.src.kind == OperandKind::kMemory || in.dst.kind == OperandKind::kMemory)
                    ? make_fault_label(idx)
                    : a64::Label{0};
            if (in.dst.kind == OperandKind::kRegister) {
                load_any_op(in.src, kR, flbl);
                write_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, in.dst.high_byte, kR, kT0);
            } else {
                load_any_op(in.src, kR, flbl);
                emit_effective_address_jit(e, ra, in.dst.mem, next_rip, kB, kT0);
                emit_inline_tlb_write(e, kB, kR, bytes, flbl, write_stubs);
            }
            return TranslateError::kNone;
        }

        case Opcode::kMovsx: {
            const a64::Label flbl =
                (in.src.kind == OperandKind::kMemory) ? make_fault_label(idx) : a64::Label{0};
            load_any_op(in.src, kR, flbl);
            e.sbfx(a64::Width::k64, kR, kR, 0, static_cast<unsigned>(op_bits(in.src.size)));
            write_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, false, kR, kT0);
            return TranslateError::kNone;
        }

        case Opcode::kCdqe: {
            load_gpr_rb(e, &ra, Gpr::kRax, kA);
            e.sbfx(a64::Width::k64, kR, kA, 0, static_cast<unsigned>(bits / 2));
            write_gpr_sized_jit(e, ra, Gpr::kRax, in.dst.size, false, kR, kT0);
            return TranslateError::kNone;
        }

        case Opcode::kCqo: {
            load_gpr_rb(e, &ra, Gpr::kRax, kA);
            if (bits < 64) {
                e.sbfx(a64::Width::k64, kR, kA, 0, static_cast<unsigned>(bits));
                e.asr_imm(a64::Width::k64, kR, kR, 63);
            } else {
                e.asr_imm(a64::Width::k64, kR, kA, 63);
            }
            write_gpr_sized_jit(e, ra, Gpr::kRdx, in.dst.size, false, kR, kT0);
            return TranslateError::kNone;
        }

        case Opcode::kLea: {
            emit_effective_address_jit(e, ra, in.src.mem, next_rip, kR, kT0);
            write_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, false, kR, kT0);
            return TranslateError::kNone;
        }

        case Opcode::kAdd:
        case Opcode::kSub:
        case Opcode::kCmp:
        case Opcode::kAnd:
        case Opcode::kOr:
        case Opcode::kXor:
        case Opcode::kTest: {
            if (bits == 32 || bits == 64) {
                const bool writes_dst = (in.opcode != Opcode::kCmp && in.opcode != Opcode::kTest);
                const bool is_sub = (in.opcode == Opcode::kSub || in.opcode == Opcode::kCmp);
                const bool is_logical = (in.opcode == Opcode::kAnd || in.opcode == Opcode::kOr ||
                                         in.opcode == Opcode::kXor || in.opcode == Opcode::kTest);
                const a64::Label flbl =
                    (in.dst.kind == OperandKind::kMemory || in.src.kind == OperandKind::kMemory)
                        ? make_fault_label(idx)
                        : a64::Label{0};

                if (in.dst.kind == OperandKind::kMemory) {
                    emit_effective_address_jit(e, ra, in.dst.mem, next_rip, kB, kT0);
                    if (writes_dst) {
                        e.str(a64::Width::k64, kB, a64::kSp, kScratchSlot1);
                    }
                    emit_inline_tlb_read(e, kB, kA, bytes, flbl, read_stubs);
                    load_any_op(in.src, kB, flbl);
                } else {
                    // Load memory source FIRST so kA isn't clobbered across cold BLR!
                    load_any_op(in.src, kB, flbl);
                    read_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, false, kA);
                }

                if (is_logical) {
                    switch (in.opcode) {
                    case Opcode::kAnd:
                    case Opcode::kTest:
                        e.and_(w, kR, kA, kB);
                        break;
                    case Opcode::kOr:
                        e.orr(w, kR, kA, kB);
                        break;
                    default:
                        e.eor(w, kR, kA, kB);
                        break;
                    }
                    if (lflags != 0) {
                        emit_flags_logical(e, w, lflags, kR);
                        emit_pf_af_store(e, lflags, kR, a64::kXzr); // AF is 0
                    }
                } else {
                    const bool pfaf_live = (lflags & kFbPfAf) != 0;
                    if (pfaf_live) {
                        e.eor(a64::Width::k64, kT2, kA, kB);
                    }
                    if (is_sub) {
                        e.subs(w, kR, kA, kB);
                    } else {
                        e.adds(w, kR, kA, kB);
                    }
                    if (pfaf_live) {
                        e.eor(a64::Width::k64, kT2, kT2, kR); // bit 4 = AF
                    }
                    const auto eager_mask = static_cast<std::uint8_t>(lflags & ~kFbPfAf);
                    if (eager_mask != 0) {
                        emit_flags_from_nzcv(e, is_sub, eager_mask, kT2, kR);
                    }
                    emit_pf_af_store(e, lflags, kR, kT2);
                }

                if (writes_dst) {
                    if (in.dst.kind == OperandKind::kMemory) {
                        e.ldr(a64::Width::k64, kB, a64::kSp, kScratchSlot1);
                        emit_inline_tlb_write(e, kB, kR, bytes, flbl, write_stubs);
                    } else {
                        write_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, false, kR, kT0);
                    }
                }
                return TranslateError::kNone;
            }
            // 8/16-bit CMP / TEST / AND / OR / XOR, inlined with exact six-flag
            // semantics.
            //
            // These used to reach the per-instruction C++ helper whenever more
            // than ZF/CF was live, and measurement said that is where almost all
            // helper calls come from: on a real guest (551k retired instructions)
            // 95% of them were 8-bit `cmp`/`test` with all six flags live, because
            // a block that ends without a flag-consuming branch leaves every flag
            // architecturally live. Byte compares are not exotic - a decompressor,
            // a string routine or any `char` classification is full of them - so
            // paying a full GuestState-synchronising call for each was expensive.
            //
            // The arithmetic is the 32/64-bit case's, with the flags fixed up
            // afterwards by emit_flags_wide: CF and ZF survive truncation, SF/OF
            // do not.
            const bool writes_dst = (in.opcode != Opcode::kCmp && in.opcode != Opcode::kTest);
            const bool is_logical = (in.opcode == Opcode::kAnd || in.opcode == Opcode::kOr ||
                                     in.opcode == Opcode::kXor || in.opcode == Opcode::kTest);
            const bool is_sub = (in.opcode == Opcode::kCmp);
            // SF/OF/AF/PF all read the truncated result, so it only costs
            // anything when one of them is live or the result is stored.
            const bool need_result = writes_dst || (lflags & (kFbSf | kFbOf | kFbAf | kFbPf)) != 0;

            const a64::Label flbl =
                (in.dst.kind == OperandKind::kMemory || in.src.kind == OperandKind::kMemory)
                    ? make_fault_label(idx)
                    : a64::Label{0};
            if (in.dst.kind == OperandKind::kMemory) {
                emit_effective_address_jit(e, ra, in.dst.mem, next_rip, kB, kT0);
                if (writes_dst) {
                    e.str(a64::Width::k64, kB, a64::kSp, kScratchSlot1);
                }
                emit_inline_tlb_read(e, kB, kA, bytes, flbl, read_stubs);
                load_any_op(in.src, kB, flbl);
            } else {
                load_any_op(in.src, kB, flbl);
                read_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, in.dst.high_byte, kA);
            }
            e.ubfx(a64::Width::k32, kB, kB, 0, static_cast<unsigned>(bits));
            if (is_logical) {
                switch (in.opcode) {
                case Opcode::kAnd:
                case Opcode::kTest:
                    e.and_(a64::Width::k32, kR, kA, kB);
                    break;
                case Opcode::kOr:
                    e.orr(a64::Width::k32, kR, kA, kB);
                    break;
                default:
                    e.eor(a64::Width::k32, kR, kA, kB);
                    break;
                }
            } else {
                e.subs(a64::Width::k32, kR, kA, kB);
            }
            if (need_result) {
                if (bits == 8) {
                    e.ubfx(a64::Width::k32, kR, kR, 0, 8);
                } else {
                    e.ubfx(a64::Width::k32, kR, kR, 0, 16);
                }
            }
            emit_flags_wide(e, is_logical, is_sub, lflags, static_cast<unsigned>(bits), kR, kA, kB,
                            kT1, kT2);
            if ((lflags & kFbPfAf) != 0) {
                // Record the pending PF/AF descriptor. Bit 4 of (a ^ b ^ r) lies
                // inside the 8/16-bit operand, so it survives truncation, and the
                // parity input is the low byte of the truncated result. Computed
                // after emit_flags_wide, which uses kT1 as its own scratch.
                if (!is_logical) {
                    e.eor(a64::Width::k32, kT1, kA, kB);
                    e.eor(a64::Width::k32, kT1, kT1, kR);
                }
                emit_pf_af_store(e, lflags, kR, is_logical ? a64::kXzr : kT1);
            }
            if (writes_dst) {
                if (in.dst.kind == OperandKind::kMemory) {
                    e.ldr(a64::Width::k64, kB, a64::kSp, kScratchSlot1);
                    emit_inline_tlb_write(e, kB, kR, bytes, flbl, write_stubs);
                } else {
                    write_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, in.dst.high_byte, kR, kT0);
                }
            }
            return TranslateError::kNone;
        }

        case Opcode::kNot: {
            if (bits == 32 || bits == 64 || in.dst.kind == OperandKind::kRegister) {
                const a64::Label flbl =
                    (in.dst.kind == OperandKind::kMemory) ? make_fault_label(idx) : a64::Label{0};
                if (in.dst.kind == OperandKind::kMemory) {
                    emit_effective_address_jit(e, ra, in.dst.mem, next_rip, kB, kT0);
                    e.str(a64::Width::k64, kB, a64::kSp, kScratchSlot1);
                    emit_inline_tlb_read(e, kB, kA, bytes, flbl, read_stubs);
                    e.mvn(w, kR, kA);
                    e.ldr(a64::Width::k64, kB, a64::kSp, kScratchSlot1);
                    emit_inline_tlb_write(e, kB, kR, bytes, flbl, write_stubs);
                } else {
                    read_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, in.dst.high_byte, kA);
                    e.mvn(w, kR, kA);
                    write_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, in.dst.high_byte, kR, kT0);
                }
                return TranslateError::kNone;
            }
            break;
        }

        case Opcode::kNeg:
        case Opcode::kInc:
        case Opcode::kDec: {
            if (bits == 32 || bits == 64) {
                const a64::Label flbl =
                    (in.dst.kind == OperandKind::kMemory) ? make_fault_label(idx) : a64::Label{0};
                if (in.dst.kind == OperandKind::kMemory) {
                    emit_effective_address_jit(e, ra, in.dst.mem, next_rip, kB, kT0);
                    e.str(a64::Width::k64, kB, a64::kSp, kScratchSlot1);
                    emit_inline_tlb_read(e, kB, kA, bytes, flbl, read_stubs);
                } else {
                    read_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, false, kA);
                }
                if (in.opcode == Opcode::kNeg) {
                    const bool pfaf_live = (lflags & kFbPfAf) != 0;
                    e.subs(w, kR, a64::kXzr, kA);
                    if (pfaf_live) {
                        e.eor(a64::Width::k64, kT2, kA, kR); // 0 ^ a ^ r; bit 4 = AF
                    }
                    const auto eager_mask = static_cast<std::uint8_t>(lflags & ~kFbPfAf);
                    if (eager_mask != 0) {
                        emit_flags_from_nzcv(e, /*is_sub=*/true, eager_mask, kT2, kR);
                    }
                    emit_pf_af_store(e, lflags, kR, kT2);
                } else {
                    const bool is_sub = (in.opcode == Opcode::kDec);
                    const bool pfaf_live = (lflags & kFbPfAf) != 0;
                    if (is_sub) {
                        e.subs_imm(w, kR, kA, 1);
                    } else {
                        e.adds_imm(w, kR, kA, 1);
                    }
                    if (pfaf_live) {
                        e.eor(a64::Width::k64, kT2, kA, kR); // a ^ 1 ^ r; bit 4 = AF
                    }
                    const auto eager_mask = static_cast<std::uint8_t>(lflags & ~kFbPfAf);
                    if (eager_mask != 0) {
                        emit_flags_from_nzcv(e, is_sub, eager_mask, kT2, kR);
                    }
                    emit_pf_af_store(e, lflags, kR, kT2);
                }
                if (in.dst.kind == OperandKind::kMemory) {
                    e.ldr(a64::Width::k64, kB, a64::kSp, kScratchSlot1);
                    emit_inline_tlb_write(e, kB, kR, bytes, flbl, write_stubs);
                } else {
                    write_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, false, kR, kT0);
                }
                return TranslateError::kNone;
            }
            break;
        }

        case Opcode::kShl:
        case Opcode::kShr:
        case Opcode::kSar:
        case Opcode::kRol:
        case Opcode::kRor: {
            // Fast 1-instruction ARMv8 lowering whenever flags are dead (99% of C shifts/rotates,
            // including ALL SHA-256 rounds, array scaling, hash mixing, bitboard shifts)!
            if ((bits == 32 || bits == 64) && lflags == 0 &&
                in.dst.kind == OperandKind::kRegister) {
                read_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, false, kA);
                const unsigned mask_b = (bits == 64) ? 63u : 31u;
                if (in.src.kind == OperandKind::kImmediate) {
                    const auto cnt = static_cast<unsigned>(in.src.imm & mask_b);
                    if (cnt == 0) {
                        e.mov_reg(w, kR, kA);
                    } else {
                        switch (in.opcode) {
                        case Opcode::kShl:
                            e.lsl_imm(w, kR, kA, cnt);
                            break;
                        case Opcode::kShr:
                            e.lsr_imm(w, kR, kA, cnt);
                            break;
                        case Opcode::kSar:
                            e.asr_imm(w, kR, kA, cnt);
                            break;
                        case Opcode::kRol:
                            e.ror_imm(w, kR, kA,
                                      (static_cast<unsigned>(bits) - cnt) %
                                          static_cast<unsigned>(bits));
                            break;
                        default:
                            e.ror_imm(w, kR, kA, cnt);
                            break;
                        }
                    }
                } else {
                    read_gpr_sized_jit(e, ra, Gpr::kRcx, OperandSize::k8, false, kB);
                    switch (in.opcode) {
                    case Opcode::kShl:
                        e.lsl_reg(w, kR, kA, kB);
                        break;
                    case Opcode::kShr:
                        e.lsr_reg(w, kR, kA, kB);
                        break;
                    case Opcode::kSar:
                        e.asr_reg(w, kR, kA, kB);
                        break;
                    case Opcode::kRol:
                        e.mvn(w, kT0, kB);
                        e.add_imm(w, kT0, kT0, 1);
                        e.ror_reg(w, kR, kA, kT0);
                        break;
                    default:
                        e.ror_reg(w, kR, kA, kB);
                        break;
                    }
                }
                write_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, false, kR, kT0);
                return TranslateError::kNone;
            }
            break;
        }

        case Opcode::kImul: {
            if (in.has_src && (bits == 32 || bits == 64) && lflags == 0) {
                const Operand& lhs = in.has_aux ? in.src : in.dst;
                const Operand& rhs = in.has_aux ? in.aux : in.src;
                const a64::Label flbl =
                    (lhs.kind == OperandKind::kMemory || rhs.kind == OperandKind::kMemory)
                        ? make_fault_label(idx)
                        : a64::Label{0};
                if (lhs.kind == OperandKind::kMemory) {
                    load_any_op(lhs, kA, flbl);
                    load_any_op(rhs, kB, flbl);
                } else {
                    load_any_op(rhs, kB, flbl);
                    load_any_op(lhs, kA, flbl);
                }
                e.mul(w, kR, kA, kB);
                write_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, false, kR, kT0);
                return TranslateError::kNone;
            }
            break;
        }

        case Opcode::kSetcc: {
            emit_eval_cond_to_reg(e, in.condition, kR);
            if (in.dst.kind == OperandKind::kRegister) {
                write_gpr_sized_jit(e, ra, in.dst.reg, OperandSize::k8, in.dst.high_byte, kR, kT0);
            } else {
                const a64::Label flbl = make_fault_label(idx);
                emit_effective_address_jit(e, ra, in.dst.mem, next_rip, kB, kT0);
                emit_inline_tlb_write(e, kB, kR, 1, flbl, write_stubs);
            }
            return TranslateError::kNone;
        }

        case Opcode::kCmovcc: {
            if (bits == 32 || bits == 64) {
                const a64::Label flbl =
                    (in.src.kind == OperandKind::kMemory) ? make_fault_label(idx) : a64::Label{0};
                load_any_op(in.src, kB, flbl); // x86 rule: memory read unconditionally!
                read_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, false, kA);
                emit_eval_cond_to_reg(e, in.condition, kT0);
                e.cmp_imm(a64::Width::k32, kT0, 0);
                e.csel(w, kR, kB, kA, a64::Cond::kNe);
                write_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, false, kR, kT0);
                return TranslateError::kNone;
            }
            break;
        }

        case Opcode::kBswap: {
            read_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, false, kA);
            e.rev(w, kR, kA);
            write_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, false, kR, kT0);
            return TranslateError::kNone;
        }

        case Opcode::kBsf:
        case Opcode::kBsr: {
            if (bits == 32 || bits == 64) {
                const a64::Label flbl =
                    (in.src.kind == OperandKind::kMemory) ? make_fault_label(idx) : a64::Label{0};
                load_any_op(in.src, kB, flbl);
                if (in.rep == 0xF3) {
                    // TZCNT / LZCNT (`rbit+clz` / `clz` returns `bits` on 0 in ARMv8 hardware!)
                    if (in.opcode == Opcode::kBsf) {
                        e.rbit(w, kR, kB);
                        e.clz(w, kR, kR);
                    } else {
                        e.clz(w, kR, kB);
                    }
                    if ((lflags & kFbCf) != 0) {
                        e.cmp_imm(w, kB, 0);
                        e.cset(a64::Width::k32, kT1, a64::Cond::kEq);
                        e.strb(kT1, kState, kCfOffset);
                    }
                    if ((lflags & kFbZf) != 0) {
                        e.cmp_imm(w, kR, 0);
                        e.cset(a64::Width::k32, kT1, a64::Cond::kEq);
                        e.strb(kT1, kState, kZfOffset);
                    }
                    write_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, false, kR, kT0);
                } else {
                    // BSF / BSR: if src == 0, ZF=1 and entire 64b dst left unmodified!
                    if (in.opcode == Opcode::kBsf) {
                        e.rbit(w, kR, kB);
                        e.clz(w, kR, kR);
                    } else {
                        e.clz(w, kR, kB);
                        e.movz(a64::Width::k32, kT0, static_cast<std::uint16_t>(bits - 1));
                        e.eor(a64::Width::k32, kR, kR, kT0);
                    }
                    load_gpr_rb(e, &ra, in.dst.reg, kA);
                    e.cmp_imm(w, kB, 0);
                    if ((lflags & kFbZf) != 0) {
                        e.cset(a64::Width::k32, kT1, a64::Cond::kEq);
                        e.strb(kT1, kState, kZfOffset);
                    }
                    e.csel(a64::Width::k64, kR, kA, kR, a64::Cond::kEq);
                    store_gpr_rb(e, &ra, kR, in.dst.reg);
                }
                return TranslateError::kNone;
            }
            break;
        }

        case Opcode::kPush: {
            const a64::Label flbl = make_fault_label(idx);
            load_any_op(in.dst, kR, flbl);
            load_gpr_rb(e, &ra, Gpr::kRsp, kB);
            e.sub_imm(a64::Width::k64, kB, kB, 8);
            emit_inline_tlb_write(e, kB, kR, 8, flbl, write_stubs);
            load_gpr_rb(e, &ra, Gpr::kRsp, kB);
            e.sub_imm(a64::Width::k64, kB, kB, 8);
            store_gpr_rb(e, &ra, kB, Gpr::kRsp);
            return TranslateError::kNone;
        }

        case Opcode::kPop: {
            if (in.dst.kind == OperandKind::kRegister) {
                const a64::Label flbl = make_fault_label(idx);
                load_gpr_rb(e, &ra, Gpr::kRsp, kB);
                emit_inline_tlb_read(e, kB, kR, 8, flbl, read_stubs);
                load_gpr_rb(e, &ra, Gpr::kRsp, kA);
                e.add_imm(a64::Width::k64, kA, kA, 8);
                store_gpr_rb(e, &ra, kA, Gpr::kRsp);
                store_gpr_rb(e, &ra, kR, in.dst.reg);
                return TranslateError::kNone;
            }
            break; // `pop [mem]` handled cleanly via Tier B helper
        }

        case Opcode::kLeave: {
            const a64::Label flbl = make_fault_label(idx);
            load_gpr_rb(e, &ra, Gpr::kRbp, kB);
            emit_inline_tlb_read(e, kB, kR, 8, flbl, read_stubs);
            load_gpr_rb(e, &ra, Gpr::kRbp, kA);
            e.add_imm(a64::Width::k64, kA, kA, 8);
            store_gpr_rb(e, &ra, kA, Gpr::kRsp);
            store_gpr_rb(e, &ra, kR, Gpr::kRbp);
            return TranslateError::kNone;
        }

        case Opcode::kSse: {
            auto load_xmm_src_to_q = [&](const Operand& op, unsigned vq, a64::Label flbl) {
                if (op.kind == OperandKind::kXmm) {
                    emit_ldr_q(e, vq, kState, xmm_offset(op.xmm));
                } else {
                    emit_effective_address_jit(e, ra, op.mem, next_rip, kB, kT0);
                    e.str(a64::Width::k64, kB, a64::kSp, kScratchSlot2);
                    emit_inline_tlb_read(e, kB, kA, 8, flbl, read_stubs);
                    e.str(a64::Width::k64, kA, a64::kSp, kScratchSlot3);
                    e.ldr(a64::Width::k64, kB, a64::kSp, kScratchSlot2);
                    e.add_imm(a64::Width::k64, kB, kB, 8);
                    emit_inline_tlb_read(e, kB, kR, 8, flbl, read_stubs);
                    e.ldr(a64::Width::k64, kA, a64::kSp, kScratchSlot3);
                    e.str(a64::Width::k64, kA, a64::kSp, kScratchSlot2);
                    e.str(a64::Width::k64, kR, a64::kSp, kScratchSlot3);
                    emit_ldr_q(e, vq, a64::kSp, kScratchSlot2);
                }
            };

            switch (in.sse_op) {
            case SseOp::kMovUps: {
                if (in.dst.kind == OperandKind::kXmm && in.src.kind == OperandKind::kXmm) {
                    emit_ldr_q(e, 0, kState, xmm_offset(in.src.xmm));
                    emit_str_q(e, 0, kState, xmm_offset(in.dst.xmm));
                    return TranslateError::kNone;
                }
                const a64::Label flbl = make_fault_label(idx);
                if (in.dst.kind == OperandKind::kXmm) {
                    load_xmm_src_to_q(in.src, 0, flbl);
                    emit_str_q(e, 0, kState, xmm_offset(in.dst.xmm));
                    return TranslateError::kNone;
                }
                if (in.dst.kind == OperandKind::kMemory && in.src.kind == OperandKind::kXmm) {
                    emit_effective_address_jit(e, ra, in.dst.mem, next_rip, kB, kT0);
                    e.str(a64::Width::k64, kB, a64::kSp, kScratchSlot2);
                    e.ldp_off(kA, kR, kState, static_cast<int>(xmm_offset(in.src.xmm)));
                    e.str(a64::Width::k64, kR, a64::kSp, kScratchSlot3);
                    emit_inline_tlb_write(e, kB, kA, 8, flbl, write_stubs);
                    e.ldr(a64::Width::k64, kB, a64::kSp, kScratchSlot2);
                    e.add_imm(a64::Width::k64, kB, kB, 8);
                    e.ldr(a64::Width::k64, kR, a64::kSp, kScratchSlot3);
                    emit_inline_tlb_write(e, kB, kR, 8, flbl, write_stubs);
                    return TranslateError::kNone;
                }
                break;
            }

            case SseOp::kMovQ: {
                const a64::Label flbl =
                    (in.dst.kind == OperandKind::kMemory || in.src.kind == OperandKind::kMemory)
                        ? make_fault_label(idx)
                        : a64::Label{0};
                if (in.dst.kind == OperandKind::kXmm) {
                    if (in.src.kind == OperandKind::kXmm) {
                        e.ldr(a64::Width::k64, kA, kState, xmm_offset(in.src.xmm));
                    } else {
                        load_any_op(in.src, kA, flbl);
                    }
                    e.stp_off(kA, a64::kXzr, kState, static_cast<int>(xmm_offset(in.dst.xmm)));
                    return TranslateError::kNone;
                }
                e.ldr(a64::Width::k64, kA, kState, xmm_offset(in.src.xmm));
                if (in.dst.kind == OperandKind::kRegister) {
                    write_gpr_sized_jit(e, ra, in.dst.reg, in.dst.size, false, kA, kT0);
                } else {
                    emit_effective_address_jit(e, ra, in.dst.mem, next_rip, kB, kT0);
                    emit_inline_tlb_write(e, kB, kA, 8, flbl, write_stubs);
                }
                return TranslateError::kNone;
            }

            case SseOp::kMovD: {
                if (in.dst.kind == OperandKind::kXmm && in.src.kind != OperandKind::kXmm) {
                    const a64::Label flbl = (in.src.kind == OperandKind::kMemory)
                                                ? make_fault_label(idx)
                                                : a64::Label{0};
                    load_any_op(in.src, kA, flbl);
                    e.mov_reg(a64::Width::k32, kA, kA);
                    e.stp_off(kA, a64::kXzr, kState, static_cast<int>(xmm_offset(in.dst.xmm)));
                    return TranslateError::kNone;
                }
                if (in.dst.kind != OperandKind::kXmm && in.src.kind == OperandKind::kXmm) {
                    const a64::Label flbl = (in.dst.kind == OperandKind::kMemory)
                                                ? make_fault_label(idx)
                                                : a64::Label{0};
                    e.ldr(a64::Width::k32, kA, kState, xmm_offset(in.src.xmm));
                    if (in.dst.kind == OperandKind::kRegister) {
                        write_gpr_sized_jit(e, ra, in.dst.reg, OperandSize::k32, false, kA, kT0);
                    } else {
                        emit_effective_address_jit(e, ra, in.dst.mem, next_rip, kB, kT0);
                        emit_inline_tlb_write(e, kB, kA, 4, flbl, write_stubs);
                    }
                    return TranslateError::kNone;
                }
                break;
            }

            case SseOp::kPand:
            case SseOp::kPor:
            case SseOp::kPxor:
            case SseOp::kPandn:
            case SseOp::kPaddd:
            case SseOp::kPaddq:
            case SseOp::kPsubd:
            case SseOp::kPunpcklbw:
            case SseOp::kPunpckhbw:
            case SseOp::kPunpcklwd:
            case SseOp::kPunpckhwd:
            case SseOp::kPunpcklqdq:
            case SseOp::kPunpckhqdq:
            case SseOp::kPackuswb: {
                const a64::Label flbl =
                    (in.src.kind == OperandKind::kMemory) ? make_fault_label(idx) : a64::Label{0};
                load_xmm_src_to_q(in.src, 1, flbl);
                emit_ldr_q(e, 0, kState, xmm_offset(in.dst.xmm));
                switch (in.sse_op) {
                case SseOp::kPand:
                    e.emit_raw_u32(0x4E211C00u); // and v0.16b, v0.16b, v1.16b
                    break;
                case SseOp::kPor:
                    e.emit_raw_u32(0x4EA11C00u); // orr v0.16b, v0.16b, v1.16b
                    break;
                case SseOp::kPxor:
                    e.emit_raw_u32(0x6E211C00u); // eor v0.16b, v0.16b, v1.16b
                    break;
                case SseOp::kPandn:
                    e.emit_raw_u32(0x4E601C20u); // bic v0.16b, v1.16b, v0.16b (~v0 & v1)
                    break;
                case SseOp::kPaddd:
                    e.emit_raw_u32(0x4EA18400u); // add v0.4s, v0.4s, v1.4s
                    break;
                case SseOp::kPaddq:
                    e.emit_raw_u32(0x4EE18400u); // add v0.2d, v0.2d, v1.2d
                    break;
                case SseOp::kPsubd:
                    e.emit_raw_u32(0x6EA18400u); // sub v0.4s, v0.4s, v1.4s
                    break;
                case SseOp::kPunpcklbw:
                    e.emit_raw_u32(0x4E013800u); // zip1 v0.16b, v0.16b, v1.16b
                    break;
                case SseOp::kPunpckhbw:
                    e.emit_raw_u32(0x4E017800u); // zip2 v0.16b, v0.16b, v1.16b
                    break;
                case SseOp::kPunpcklwd:
                    e.emit_raw_u32(0x4E413800u); // zip1 v0.8h, v0.8h, v1.8h
                    break;
                case SseOp::kPunpckhwd:
                    e.emit_raw_u32(0x4E417800u); // zip2 v0.8h, v0.8h, v1.8h
                    break;
                case SseOp::kPunpcklqdq:
                    e.emit_raw_u32(0x4EC13800u); // zip1 v0.2d, v0.2d, v1.2d
                    break;
                case SseOp::kPunpckhqdq:
                    e.emit_raw_u32(0x4EC17800u); // zip2 v0.2d, v0.2d, v1.2d
                    break;
                default:                         // kPackuswb
                    e.emit_raw_u32(0x2E212800u); // sqxtun v0.8b, v0.8h
                    e.emit_raw_u32(0x6E212820u); // sqxtun2 v0.16b, v1.8h
                    break;
                }
                emit_str_q(e, 0, kState, xmm_offset(in.dst.xmm));
                return TranslateError::kNone;
            }

            case SseOp::kPsllwImm:
            case SseOp::kPsrlwImm:
            case SseOp::kPslldImm:
            case SseOp::kPsrldImm:
            case SseOp::kPsllqImm:
            case SseOp::kPsrlqImm:
            case SseOp::kPsrldqImm: {
                const auto c = static_cast<unsigned>(in.src.imm);
                if (c == 0) {
                    return TranslateError::kNone;
                }
                const unsigned max_c =
                    (in.sse_op == SseOp::kPsllwImm || in.sse_op == SseOp::kPsrlwImm ||
                     in.sse_op == SseOp::kPsrldqImm)
                        ? 16u
                        : ((in.sse_op == SseOp::kPslldImm || in.sse_op == SseOp::kPsrldImm) ? 32u
                                                                                            : 64u);
                if (c >= max_c) {
                    e.stp_off(a64::kXzr, a64::kXzr, kState,
                              static_cast<int>(xmm_offset(in.dst.xmm)));
                    return TranslateError::kNone;
                }
                emit_ldr_q(e, 0, kState, xmm_offset(in.dst.xmm));
                switch (in.sse_op) {
                case SseOp::kPsllwImm:
                    e.emit_raw_u32(0x4F105400u | (c << 16));
                    break;
                case SseOp::kPsrlwImm:
                    e.emit_raw_u32(0x6F100400u | ((16u - c) << 16));
                    break;
                case SseOp::kPslldImm:
                    e.emit_raw_u32(0x4F205400u | (c << 16));
                    break;
                case SseOp::kPsrldImm:
                    e.emit_raw_u32(0x6F200400u | ((32u - c) << 16));
                    break;
                case SseOp::kPsllqImm:
                    e.emit_raw_u32(0x4F405400u | (c << 16));
                    break;
                case SseOp::kPsrlqImm:
                    e.emit_raw_u32(0x6F400400u | ((64u - c) << 16));
                    break;
                default:                                     // kPsrldqImm (1..15)
                    e.emit_raw_u32(0x4F00E402u);             // movi v2.16b, #0
                    e.emit_raw_u32(0x6E020000u | (c << 11)); // ext v0.16b, v0.16b, v2.16b, #c
                    break;
                }
                emit_str_q(e, 0, kState, xmm_offset(in.dst.xmm));
                return TranslateError::kNone;
            }

            case SseOp::kAddSs:
            case SseOp::kSubSs:
            case SseOp::kMulSs:
            case SseOp::kDivSs:
            case SseOp::kSqrtSs:
            case SseOp::kAddSd:
            case SseOp::kSubSd:
            case SseOp::kMulSd:
            case SseOp::kDivSd:
            case SseOp::kSqrtSd:
            case SseOp::kAddPs:
            case SseOp::kSubPs:
            case SseOp::kMulPs:
            case SseOp::kDivPs:
            case SseOp::kAddPd:
            case SseOp::kSubPd:
            case SseOp::kMulPd:
            case SseOp::kDivPd: {
                if (in.src.kind != OperandKind::kXmm) {
                    break;
                }
                emit_ldr_q(e, 0, kState, xmm_offset(in.dst.xmm));
                emit_ldr_q(e, 1, kState, xmm_offset(in.src.xmm));
                switch (in.sse_op) {
                case SseOp::kAddSs:
                    e.emit_raw_u32(0x1E212802u); // fadds s2, s0, s1
                    e.emit_raw_u32(0x6E040440u); // ins v0.s[0], v2.s[0]
                    break;
                case SseOp::kSubSs:
                    e.emit_raw_u32(0x1E213802u); // fsubs s2, s0, s1
                    e.emit_raw_u32(0x6E040440u);
                    break;
                case SseOp::kMulSs:
                    e.emit_raw_u32(0x1E210802u); // fmuls s2, s0, s1
                    e.emit_raw_u32(0x6E040440u);
                    break;
                case SseOp::kDivSs:
                    e.emit_raw_u32(0x1E211802u); // fdivs s2, s0, s1
                    e.emit_raw_u32(0x6E040440u);
                    break;
                case SseOp::kSqrtSs:
                    e.emit_raw_u32(0x1E21C022u); // fsqrts s2, s1
                    e.emit_raw_u32(0x6E040440u);
                    break;
                case SseOp::kAddSd:
                    e.emit_raw_u32(0x1E612802u); // faddd d2, d0, d1
                    e.emit_raw_u32(0x6E080440u); // ins v0.d[0], v2.d[0]
                    break;
                case SseOp::kSubSd:
                    e.emit_raw_u32(0x1E613802u); // fsubd d2, d0, d1
                    e.emit_raw_u32(0x6E080440u);
                    break;
                case SseOp::kMulSd:
                    e.emit_raw_u32(0x1E610802u); // fmuld d2, d0, d1
                    e.emit_raw_u32(0x6E080440u);
                    break;
                case SseOp::kDivSd:
                    e.emit_raw_u32(0x1E611802u); // fdivd d2, d0, d1
                    e.emit_raw_u32(0x6E080440u);
                    break;
                case SseOp::kSqrtSd:
                    e.emit_raw_u32(0x1E61C022u); // fsqrtd d2, d1
                    e.emit_raw_u32(0x6E080440u);
                    break;
                case SseOp::kAddPs:
                    e.emit_raw_u32(0x4E21D400u); // fadd v0.4s, v0.4s, v1.4s
                    break;
                case SseOp::kSubPs:
                    e.emit_raw_u32(0x4EA1D400u); // fsub v0.4s, v0.4s, v1.4s
                    break;
                case SseOp::kMulPs:
                    e.emit_raw_u32(0x6E21DC00u); // fmul v0.4s, v0.4s, v1.4s
                    break;
                case SseOp::kDivPs:
                    e.emit_raw_u32(0x6E21FC00u); // fdiv v0.4s, v0.4s, v1.4s
                    break;
                case SseOp::kAddPd:
                    e.emit_raw_u32(0x4E61D400u); // fadd v0.2d, v0.2d, v1.2d
                    break;
                case SseOp::kSubPd:
                    e.emit_raw_u32(0x4EE1D400u); // fsub v0.2d, v0.2d, v1.2d
                    break;
                case SseOp::kMulPd:
                    e.emit_raw_u32(0x6E61DC00u); // fmul v0.2d, v0.2d, v1.2d
                    break;
                default:
                    e.emit_raw_u32(0x6E61FC00u); // fdiv v0.2d, v0.2d, v1.2d
                    break;
                }
                emit_str_q(e, 0, kState, xmm_offset(in.dst.xmm));
                return TranslateError::kNone;
            }

            default:
                break;
            }
            break;
        }

        default:
            break;
        }

        // Tier B in-block direct helper call for multi-word SSE2 / REP string /
        // DIV / IDIV / ADC / SBB / CMPXCHG / XADD / BT* / CPUID / 8-16b 6-flag ALU:
        // Zero block fragmentation, exact architectural state, shares hot `ctx->tlb`!
        const a64::Label flbl = make_fault_label(idx);
        const a64::Label lit_lbl = e.new_label();
        embedded_insns.push_back({lit_lbl, in});
        spill_ra();
        e.mov_imm64(kA, in.address);
        e.str(a64::Width::k64, kA, kState, kRipOffset);
        e.mov_reg(a64::Width::k64, 0, kCtxReg);
        e.mov_reg(a64::Width::k64, 1, kSavedState);
        e.adr(2, lit_lbl);
        e.ldr(a64::Width::k64, kA, kCtxReg, offsetof(ExecContext, exec_complex));
        e.blr(kA);
        e.mov_reg(a64::Width::k64, kState, kSavedState);
        reload_ra();
        e.cbz(a64::Width::k32, 0, flbl);
        return TranslateError::kNone;
    };

    // Detect single-block self-loop (`steps[0..m-2]` + `Jcc first.address`)
    // including memory-accessing loops (`sieve`, `sha256`, `quicksort_c`)!
    a64::Cond self_loop_taken_cond = a64::Cond::kAl;
    const Instruction* pre_jcc = (steps.size() >= 2) ? &steps[steps.size() - 2].insn : nullptr;
    const bool pre_jcc_fusible =
        pre_jcc != nullptr &&
        (pre_jcc->dst.size == OperandSize::k32 || pre_jcc->dst.size == OperandSize::k64) &&
        try_nzcv_cond(pre_jcc->opcode, steps.back().insn.condition, self_loop_taken_cond);
    const bool use_self_loop = steps.size() >= 2 && exit == BlockExit::kJcc &&
                               steps.back().insn.dst.imm == first.address && pre_jcc_fusible &&
                               is_classic_fast_alu(*pre_jcc);

    if (use_self_loop) {
        // Entry budget check before first iteration + reload allocated GPRs once!
        e.subs_imm(a64::Width::k64, kRemBudget, kRemBudget, block_insns);
        e.b_cond(a64::Cond::kCc, entry_budget_undo);
        reload_ra();

        constexpr a64::Reg kLoopA = 2;
        constexpr a64::Reg kLoopB = 3;
        constexpr a64::Reg kLoopRes = 4;

        const a64::Label loop_top = e.new_label();
        const a64::Label not_taken_exit = e.new_label();
        const a64::Label budget_loop_exit = e.new_label();
        const a64::Label emit_flags_common = e.new_label();

        e.bind(loop_top);
        for (std::size_t i = 0; i + 2 < steps.size(); ++i) {
            const TranslateError err = emit_cached_body_insn(i);
            if (err != TranslateError::kNone) {
                out.error = err;
                return out;
            }
        }

        // Emit `steps[m - 2]` directly into its destination register while keeping
        // pre-op operands for once-per-loop cold flag materialization!
        const Instruction& cmp_or_alu = steps[steps.size() - 2].insn;
        const bool is32 = cmp_or_alu.dst.size == OperandSize::k32;
        const a64::Width w = is32 ? a64::Width::k32 : a64::Width::k64;
        const int bits = is32 ? 32 : 64;
        const bool is_logical =
            cmp_or_alu.opcode == Opcode::kAnd || cmp_or_alu.opcode == Opcode::kOr ||
            cmp_or_alu.opcode == Opcode::kXor || cmp_or_alu.opcode == Opcode::kTest;
        const bool is_sub = cmp_or_alu.opcode == Opcode::kSub ||
                            cmp_or_alu.opcode == Opcode::kCmp || cmp_or_alu.opcode == Opcode::kDec;
        const bool is_unary =
            cmp_or_alu.opcode == Opcode::kInc || cmp_or_alu.opcode == Opcode::kDec;
        const bool writes_dst =
            cmp_or_alu.opcode != Opcode::kCmp && cmp_or_alu.opcode != Opcode::kTest;
        const std::uint8_t exit_mask = flags_written(cmp_or_alu.opcode);

        load_gpr_rb(e, &ra, cmp_or_alu.dst.reg, kLoopA);
        const a64::Reg R =
            (writes_dst && ra.mapped(cmp_or_alu.dst.reg)) ? ra.host(cmp_or_alu.dst.reg) : kLoopRes;

        std::uint64_t imm_val = 0;
        std::uint32_t imm12 = 0;
        bool shift12 = false;
        bool folded_imm = false;
        if (is_unary) {
            imm_val = 1;
            imm12 = 1;
            folded_imm = true;
        } else if (!is_logical && cmp_or_alu.src.kind == OperandKind::kImmediate) {
            imm_val = is32 ? (cmp_or_alu.src.imm & 0xFFFFFFFFULL) : cmp_or_alu.src.imm;
            folded_imm = encode_imm12(imm_val, imm12, shift12);
        }

        if (is_logical) {
            const TranslateError src_err = load_source(e, cmp_or_alu.src, kLoopB, bits, &ra);
            if (src_err != TranslateError::kNone) {
                out.error = src_err;
                return out;
            }
            switch (cmp_or_alu.opcode) {
            case Opcode::kAnd:
            case Opcode::kTest:
                e.and_(w, R, kLoopA, kLoopB);
                break;
            case Opcode::kOr:
                e.orr(w, R, kLoopA, kLoopB);
                break;
            default:
                e.eor(w, R, kLoopA, kLoopB);
                break;
            }
            e.cmp_imm(w, R, 0);
        } else if (folded_imm) {
            if (is_sub) {
                e.subs_imm(w, R, kLoopA, imm12, shift12);
            } else {
                e.adds_imm(w, R, kLoopA, imm12, shift12);
            }
        } else {
            const TranslateError src_err = load_source(e, cmp_or_alu.src, kLoopB, bits, &ra);
            if (src_err != TranslateError::kNone) {
                out.error = src_err;
                return out;
            }
            if (is_sub) {
                e.subs(w, R, kLoopA, kLoopB);
            } else {
                e.adds(w, R, kLoopA, kLoopB);
            }
        }

        if (writes_dst && R == kLoopRes) {
            store_gpr(e, kLoopRes, cmp_or_alu.dst.reg);
        }

        // Not-taken exit (loop finished naturally):
        const auto inv_cond =
            static_cast<a64::Cond>(static_cast<std::uint32_t>(self_loop_taken_cond) ^ 1u);
        e.b_cond(inv_cond, not_taken_exit);

        // Taken backedge: check remaining step budget BEFORE clobbering NZCV if empty!
        // Look how clean: compare `x26 >= block_insns` ONLY after testing `cbz`? Even simpler:
        // `cmp_imm` WILL touch NZCV ONLY if we run it, whereas on hot path the VERY NEXT loop
        // iteration immediately overwrites NZCV! So ONLY if `x26 < block_insns` (cold once-per-run
        // budget cutoff!) do we NOT want NZCV touched before materializing flags — so test budget
        // using a non-flag-clobbering check? NO — save NZCV ONLY on the cold path BEFORE `subs`?!
        // Even better: materialize flags FIRST ONLY when exiting by testing `subs x26` AFTER
        // saving `xLoopA/xLoopB` — wait: we HAVE `xLoopA`, `xLoopB`, AND `R` IN REGISTERS!! WE CAN
        // RE-RUN THE 1-INSTRUCTION `subs_imm` / `adds_imm` / `cmp_imm` RIGHT AT `budget_loop_exit`
        // IN ONE INSTRUCTION TO REGENERATE EXACT HARDWARE `NZCV` WITHOUT TOUCHING HOT LOOP!!
        e.subs_imm(a64::Width::k64, kRemBudget, kRemBudget, block_insns);
        e.b_cond(a64::Cond::kCs, loop_top); // HOT BACKEDGE: 2 INSTRUCTIONS TOTAL!!

        // Cold budget-exhausted exit during taken backedge: undo budget & regenerate exact NZCV!
        e.bind(budget_loop_exit);
        e.add_imm(a64::Width::k64, kRemBudget, kRemBudget, block_insns);
        if (is_logical) {
            e.cmp_imm(w, R, 0);
        } else if (folded_imm) {
            if (is_sub) {
                e.subs_imm(w, kT3, kLoopA, imm12, shift12);
            } else {
                e.adds_imm(w, kT3, kLoopA, imm12, shift12);
            }
        } else {
            if (is_sub) {
                e.subs(w, kT3, kLoopA, kLoopB);
            } else {
                e.adds(w, kT3, kLoopA, kLoopB);
            }
        }
        e.mov_imm64(kA, first.address); // next PC is loop start!
        e.b(emit_flags_common);

        // Cold natural loop exit (`not_taken_exit`): NZCV is ALREADY intact!
        e.bind(not_taken_exit);
        e.mov_imm64(kA, target); // next PC is fallthrough past Jcc!

        e.bind(emit_flags_common);
        const auto loop_eager_mask = static_cast<std::uint8_t>(exit_mask & ~kFbPfAf);
        if (is_logical) {
            emit_flags_logical(e, w, loop_eager_mask, R);
            emit_pf_af_store(e, exit_mask, R, a64::kXzr); // logical: AF is 0
        } else if (folded_imm) {
            const bool af_from_xor = (exit_mask & kFbPfAf) != 0 && (imm_val & 0xFULL) != 0;
            if (af_from_xor) {
                e.eor(a64::Width::k64, kT2, kLoopA, R);
                if ((imm_val & 0x10ULL) != 0) {
                    e.mvn(a64::Width::k32, kT2, kT2);
                }
            }
            if (loop_eager_mask != 0) {
                emit_flags_from_nzcv(e, is_sub, loop_eager_mask, kT2, R);
            }
            emit_pf_af_store(e, exit_mask, R, af_from_xor ? kT2 : a64::kXzr);
        } else {
            const bool pfaf_live = (exit_mask & kFbPfAf) != 0;
            if (pfaf_live) {
                e.eor(a64::Width::k64, kT2, kLoopA, kLoopB);
                e.eor(a64::Width::k64, kT2, kT2, R);
            }
            if (loop_eager_mask != 0) {
                emit_flags_from_nzcv(e, is_sub, loop_eager_mask, kT2, R);
            }
            emit_pf_af_store(e, exit_mask, R, kT2);
        }
        spill_ra();
        e.br(kDispatchReg); // chain directly into post-loop block!
    } else {
        // Standard multi-block chained execution:
        e.subs_imm(a64::Width::k64, kRemBudget, kRemBudget, block_insns);
        e.b_cond(a64::Cond::kCc, entry_budget_undo);
        reload_ra();

        for (std::size_t idx = 0; idx < steps.size(); ++idx) {
            const IrInsn& step = steps[idx];
            const Opcode op = step.insn.opcode;
            const std::uint64_t next_rip = step.insn.address + step.insn.length;

            if (op == Opcode::kHlt) {
                spill_ra();
                if (idx > 0) {
                    e.mov_imm64(kA, step.insn.address);
                    e.str(a64::Width::k64, kA, kState, kRipOffset);
                }
                e.movz(a64::Width::k64, 0, static_cast<std::uint16_t>(BlockResult::kHalted));
                e.ldr(a64::Width::k64, kA, kCtxReg, offsetof(ExecContext, jit_exit_epilogue));
                e.br(kA);
                break;
            }
            if (op == Opcode::kSyscall) {
                spill_ra();
                e.mov_imm64(kA, next_rip);
                e.str(a64::Width::k64, kA, kState, kRipOffset);
                e.movz(a64::Width::k64, 0, static_cast<std::uint16_t>(BlockResult::kSyscall));
                e.ldr(a64::Width::k64, kA, kCtxReg, offsetof(ExecContext, jit_exit_epilogue));
                e.br(kA);
                break;
            }
            if (op == Opcode::kJmp) {
                if (step.insn.dst.kind == OperandKind::kImmediate) {
                    spill_ra();
                    e.mov_imm64(kA, step.insn.dst.imm);
                } else if (step.insn.dst.kind == OperandKind::kRegister) {
                    load_gpr_rb(e, &ra, step.insn.dst.reg, kA);
                    spill_ra();
                } else {
                    const a64::Label flbl = make_fault_label(idx);
                    emit_effective_address_jit(e, ra, step.insn.dst.mem, next_rip, kB, kT0);
                    emit_inline_tlb_read(e, kB, kA, 8, flbl, read_stubs);
                    spill_ra();
                }
                if (step.insn.dst.kind == OperandKind::kImmediate) {
                    // Record the site before the branch word: the cache may
                    // repoint it straight at the successor's entry. rip is
                    // written here rather than relying on the dispatcher to do
                    // it, so a linked exit leaves GuestState exactly as the
                    // dispatch path would have (faults report the right PC).
                    e.str(a64::Width::k64, kA, kState, kRipOffset);
                    if (out.exit_links.size() < kMaxExitLinks) {
                        out.exit_links.push_back(
                            {static_cast<std::uint64_t>(e.count()) * 4, step.insn.dst.imm});
                    }
                }
                e.br(kDispatchReg);
                break;
            }
            if (op == Opcode::kCall) {
                const a64::Label flbl = make_fault_label(idx);
                // 1. Evaluate call target into scratch slot 1 (safe across SoftTlb slow path!).
                if (step.insn.dst.kind == OperandKind::kImmediate) {
                    e.mov_imm64(kA, step.insn.dst.imm);
                } else if (step.insn.dst.kind == OperandKind::kRegister) {
                    load_gpr_rb(e, &ra, step.insn.dst.reg, kA);
                } else {
                    emit_effective_address_jit(e, ra, step.insn.dst.mem, next_rip, kB, kT0);
                    emit_inline_tlb_read(e, kB, kA, 8, flbl, read_stubs);
                }
                e.str(a64::Width::k64, kA, a64::kSp, kScratchSlot1);
                // 2. Push 64-bit `next_rip` onto guest stack `[rsp - 8]`.
                load_gpr_rb(e, &ra, Gpr::kRsp, kB);
                e.sub_imm(a64::Width::k64, kB, kB, 8);
                e.mov_imm64(kR, next_rip);
                emit_inline_tlb_write(e, kB, kR, 8, flbl, write_stubs);
                // Reload `new_rsp` in case cold write stub clobbered caller-saved `kB`!
                load_gpr_rb(e, &ra, Gpr::kRsp, kB);
                e.sub_imm(a64::Width::k64, kB, kB, 8);
                store_gpr_rb(e, &ra, kB, Gpr::kRsp);
                spill_ra();
                e.ldr(a64::Width::k64, kA, a64::kSp, kScratchSlot1);
                // P10: a `call`'s destination, when it is an immediate, is a
                // statically known block start, so the exit is linkable exactly
                // like a `jmp` (P7) - the callee entry needs the same `rip` store
                // and nothing else. This removes the shared dispatcher
                // (11 instructions + two jump-table loads) from every direct
                // call. Indirect calls and `ret` keep the dispatcher: their
                // destination is only known at run time.
                e.str(a64::Width::k64, kA, kState, kRipOffset);
                if (step.insn.dst.kind == OperandKind::kImmediate &&
                    out.exit_links.size() < kMaxExitLinks) {
                    out.exit_links.push_back(
                        {static_cast<std::uint64_t>(e.count()) * 4, step.insn.dst.imm});
                }
                e.br(kDispatchReg);
                break;
            }
            if (op == Opcode::kRet) {
                const a64::Label flbl = make_fault_label(idx);
                const a64::Label top_halt = e.new_label();
                load_gpr_rb(e, &ra, Gpr::kRsp, kB);
                e.cbz(a64::Width::k64, kB, top_halt);
                emit_inline_tlb_read(e, kB, kA, 8, flbl, read_stubs);
                e.cbz(a64::Width::k64, kA, top_halt);
                load_gpr_rb(e, &ra, Gpr::kRsp, kB);
                const auto pop_bytes = static_cast<std::uint64_t>(8ULL + step.insn.dst.imm);
                std::uint32_t imm12 = 0;
                bool sh12 = false;
                if (encode_imm12(pop_bytes, imm12, sh12)) {
                    e.add_imm(a64::Width::k64, kB, kB, imm12, sh12);
                } else {
                    e.mov_imm64(kT0, pop_bytes);
                    e.add(a64::Width::k64, kB, kB, kT0);
                }
                store_gpr_rb(e, &ra, kB, Gpr::kRsp);
                spill_ra();
                e.br(kDispatchReg); // `kA` holds `ret_addr` -> jump straight to caller!

                e.bind(top_halt);
                spill_ra();
                e.mov_imm64(kA, step.insn.address);
                e.str(a64::Width::k64, kA, kState, kRipOffset);
                e.movz(a64::Width::k64, 0, static_cast<std::uint16_t>(BlockResult::kHalted));
                e.ldr(a64::Width::k64, kA, kCtxReg, offsetof(ExecContext, jit_exit_epilogue));
                e.br(kA);
                break;
            }
            if (op == Opcode::kJcc) {
                // `spill_ra()` only emits `STR`, which preserves hardware `NZCV`!
                // Using `CSEL` instead of an internal `B.cond` keeps every guest block a
                // single straight-line AArch64 basic block (zero internal host branches!)
                // while saving 2–3 words per conditional exit.
                spill_ra();
                const std::uint64_t taken_pc = step.insn.dst.imm;
                a64::Cond fused = a64::Cond::kAl;
                bool prev_fusible = false;
                if (idx > 0) {
                    const Instruction& prev = steps[idx - 1].insn;
                    const bool prev_wrote_mem = prev.dst.kind == OperandKind::kMemory &&
                                                prev.opcode != Opcode::kCmp &&
                                                prev.opcode != Opcode::kTest;
                    prev_fusible =
                        !prev_wrote_mem &&
                        (prev.dst.size == OperandSize::k32 || prev.dst.size == OperandSize::k64) &&
                        try_nzcv_cond(prev.opcode, step.insn.condition, fused);
                }
                if (!prev_fusible) {
                    emit_eval_cond_to_reg(e, step.insn.condition, kT0);
                    e.cmp_imm(a64::Width::k32, kT0, 0);
                    fused = a64::Cond::kNe;
                }
                // Two exits rather than one `CSEL`-selected exit. The CSEL form is
                // two words shorter, but it funnels both successors through a
                // single branch word, and a single word can only ever be repointed
                // at one of them - so it keeps every conditional transition going
                // through the shared dispatcher. Splitting the paths costs two
                // words and is what lets the cache link *both* sides of a `Jcc`
                // (audit §26 row 5), which is the commonest terminator in real
                // code. Unlinked, the executed cost is unchanged: the dispatcher
                // branch still runs, exactly as before.
                const a64::Label fallthrough_path = e.new_label();
                e.b_cond(invert_cond(fused), fallthrough_path);
                e.mov_imm64(kA, taken_pc);
                e.str(a64::Width::k64, kA, kState, kRipOffset);
                if (out.exit_links.size() < kMaxExitLinks) {
                    out.exit_links.push_back({static_cast<std::uint64_t>(e.count()) * 4, taken_pc});
                }
                e.br(kDispatchReg);
                e.bind(fallthrough_path);
                e.mov_imm64(kA, next_rip);
                e.str(a64::Width::k64, kA, kState, kRipOffset);
                if (out.exit_links.size() < kMaxExitLinks) {
                    out.exit_links.push_back({static_cast<std::uint64_t>(e.count()) * 4, next_rip});
                }
                e.br(kDispatchReg);
                break;
            }

            const TranslateError err = emit_cached_body_insn(idx);
            if (err != TranslateError::kNone) {
                out.error = err;
                return out;
            }
        }

        if (exit == BlockExit::kFallThrough) {
            spill_ra();
            e.mov_imm64(kA, target);
            e.str(a64::Width::k64, kA, kState, kRipOffset);
            if (out.exit_links.size() < kMaxExitLinks) {
                out.exit_links.push_back({static_cast<std::uint64_t>(e.count()) * 4, target});
            }
            e.br(kDispatchReg);
        }
    }

    // --- Out-of-line cold stubs (never reached on steady-state hot path) ---

    // 1. Entry budget exhaustion (`x26` underflowed before touching any guest state)
    e.bind(entry_budget_undo);
    e.add_imm(a64::Width::k64, kRemBudget, kRemBudget, block_insns);
    e.br(29);

    // 2. Out-of-line SoftTlb read misses
    for (const SlowReadStub& s : read_stubs) {
        e.bind(s.slow_lbl);
        e.str(a64::Width::k64, a64::kXzr, a64::kSp, kScratchSlot0);
        e.mov_reg(a64::Width::k64, 1, s.addr_reg);
        e.mov_reg(a64::Width::k64, 0, kCtxReg);
        e.add_imm(a64::Width::k64, 2, a64::kSp, kScratchSlot0);
        e.movz(a64::Width::k64, 3, static_cast<std::uint16_t>(s.bytes));
        e.ldr(a64::Width::k64, kA, kCtxReg, offsetof(ExecContext, mem_read_slow));
        e.blr(kA);
        e.mov_reg(a64::Width::k64, kState, kSavedState);
        e.ldr(a64::Width::k64, s.out_reg, a64::kSp, kScratchSlot0);
        e.cbz(a64::Width::k32, 0, s.fault_lbl);
        e.b(s.done_lbl);
    }

    // 3. Out-of-line SoftTlb write misses
    for (const SlowWriteStub& s : write_stubs) {
        e.bind(s.slow_lbl);
        e.str(a64::Width::k64, s.val_reg, a64::kSp, kScratchSlot0);
        e.mov_reg(a64::Width::k64, 1, s.addr_reg);
        e.mov_reg(a64::Width::k64, 0, kCtxReg);
        e.add_imm(a64::Width::k64, 2, a64::kSp, kScratchSlot0);
        e.movz(a64::Width::k64, 3, static_cast<std::uint16_t>(s.bytes));
        e.ldr(a64::Width::k64, kA, kCtxReg, offsetof(ExecContext, mem_write_slow));
        e.blr(kA);
        e.mov_reg(a64::Width::k64, kState, kSavedState);
        e.cbz(a64::Width::k32, 0, s.fault_lbl);
        e.b(s.done_lbl);
    }

    // 4. Out-of-line architectural fault exits (exact instruction-granular RIP + steps!)
    for (const FaultStub& f : fault_stubs) {
        e.bind(f.fault_lbl);
        spill_ra();
        e.add_imm(a64::Width::k64, kRemBudget, kRemBudget, f.unretired_insns);
        e.mov_imm64(kA, f.fault_pc);
        e.str(a64::Width::k64, kA, kState, kRipOffset);
        e.movz(a64::Width::k64, 0, static_cast<std::uint16_t>(BlockResult::kFault));
        e.ldr(a64::Width::k64, kA, kCtxReg, offsetof(ExecContext, jit_exit_epilogue));
        e.br(kA);
    }

    // 5. 8-byte-aligned read-only embedded `Instruction` structs for Tier B helpers
    if (!embedded_insns.empty()) {
        if ((e.count() & 1u) != 0) {
            e.nop(); // ensure 8-byte alignment for `alignof(Instruction) == 8` (UBSan safe!)
        }
        static_assert(sizeof(Instruction) % 4 == 0);
        for (const EmbeddedInsn& emb : embedded_insns) {
            e.bind(emb.label);
            std::uint32_t words[sizeof(Instruction) / 4]{};
            std::memcpy(words, &emb.insn, sizeof(Instruction));
            for (const std::uint32_t word : words) {
                e.emit_raw_u32(word);
            }
        }
    }

    out.exit = exit;
    out.target = target;
    out.alternate = alternate;
    out.has_alternate = (exit == BlockExit::kJcc);
    out.guest_instruction_count = steps.size();
    out.code = e.bytes();
    out.instruction_count = e.count();
    return out;
}

} // namespace hyperion
