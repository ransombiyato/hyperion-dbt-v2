// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// x86-64 guest architectural state.
//
// This is the state the reference interpreter (Phase 2) and, later, the
// AArch64 code generator (Phase 3) must agree on. It is deliberately a plain
// data structure: no hidden laziness, no implicit normalisation, so that two
// implementations can be compared field by field in a differential test.
#ifndef HYPERION_GUEST_STATE_HPP
#define HYPERION_GUEST_STATE_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace hyperion {

// The sixteen general-purpose registers, in x86-64 encoding order. The order
// matters: it is the value stored in ModRM/REX fields, and indexing by it
// keeps the decoder free of lookup tables.
enum class Gpr : std::uint8_t {
    kRax = 0,
    kRcx = 1,
    kRdx = 2,
    kRbx = 3,
    kRsp = 4,
    kRbp = 5,
    kRsi = 6,
    kRdi = 7,
    kR8 = 8,
    kR9 = 9,
    kR10 = 10,
    kR11 = 11,
    kR12 = 12,
    kR13 = 13,
    kR14 = 14,
    kR15 = 15,
};

inline constexpr std::size_t kGprCount = 16;
inline constexpr std::size_t kXmmCount = 16;

// 128-bit SSE2 register value (`XMM0..XMM15`), stored in little-endian lane order
// (`lo` = bits 63..0, `hi` = bits 127..64).
struct alignas(16) XmmReg {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;

    friend bool operator==(const XmmReg& a, const XmmReg& b) {
        return a.lo == b.lo && a.hi == b.hi;
    }
    friend bool operator!=(const XmmReg& a, const XmmReg& b) { return !(a == b); }
};

// Status flags. x86-64 RFLAGS holds more (IF, DF, IOPL, ...); this project
// models only the flags that integer ALU operations produce. Keeping the set
// explicit means "unmodelled" is visible rather than accidentally zero.
struct Flags {
    bool cf = false; // carry
    bool pf = false; // parity (low byte)
    bool af = false; // auxiliary carry (bit 4)
    bool zf = false; // zero
    bool sf = false; // sign (bit 63)
    bool of = false; // signed overflow

    friend bool operator==(const Flags& a, const Flags& b) {
        return a.cf == b.cf && a.pf == b.pf && a.af == b.af && a.zf == b.zf && a.sf == b.sf &&
               a.of == b.of;
    }
    friend bool operator!=(const Flags& a, const Flags& b) { return !(a == b); }
};

std::string to_string(const Flags& flags);

// RFLAGS bit positions, for converting to and from the real architectural
// encoding. Bit 1 is always set on a real x86-64 CPU; it is not a flag.
inline constexpr std::uint64_t kRflagsBitCf = 1ULL << 0;
inline constexpr std::uint64_t kRflagsBitPf = 1ULL << 2;
inline constexpr std::uint64_t kRflagsBitAf = 1ULL << 4;
inline constexpr std::uint64_t kRflagsBitZf = 1ULL << 6;
inline constexpr std::uint64_t kRflagsBitSf = 1ULL << 7;
inline constexpr std::uint64_t kRflagsBitOf = 1ULL << 11;
inline constexpr std::uint64_t kRflagsBitReserved1 = 1ULL << 1;

// Only the six modelled flags; every other bit is left zero. Use this when
// comparing against a native RFLAGS captured from real hardware, and mask the
// native value with kRflagsModelMask first.
inline constexpr std::uint64_t kRflagsModelMask =
    kRflagsBitCf | kRflagsBitPf | kRflagsBitAf | kRflagsBitZf | kRflagsBitSf | kRflagsBitOf;

std::uint64_t pack_flags(const Flags& flags);
Flags unpack_flags(std::uint64_t rflags);

// Full guest architectural state.
struct GuestState {
    std::uint64_t gpr[kGprCount] = {};
    std::uint64_t rip = 0;
    Flags flags{};
    std::uint64_t fs_base = 0;    // IA32_FS_BASE (set via arch_prctl ARCH_SET_FS)
    std::uint32_t mxcsr = 0x1F80; // SSE control/status register (x86-64 reset value)
    std::uint16_t fpu_cw = 0x037F;
    std::uint16_t fpu_sw = 0;
    std::uint8_t fpu_top = 0;
    bool df = false; // RFLAGS.DF direction flag (0 = forward, 1 = backward)

    // --- Pending PF/AF descriptor (P9 stage 1; not architectural state) ------
    //
    // PF and AF are the only two flags that are expensive to materialise (PF is a
    // byte-wide parity fold) and that almost nothing reads: in a whole-program
    // profile of the benchmark guests, PF/AF are producers' most expensive flags
    // yet are *read* about seventy times less often than ZF. Generated AArch64
    // code therefore records these two bytes instead of computing the flags, and
    // materialises them at the few forced points (the JIT-to-C++ exit epilogue,
    // before a helper re-enters the interpreter, and before a consumer such as
    // JP/JNP reads PF). See docs/translator-audit.md §29.11.
    //
    // Both flags are derivable from the same two bytes of the *last* flag-writing
    // instruction, for add and subtract alike:
    //   PF = parity(result & 0xFF)
    //   AF = ((a ^ b ^ result) >> 4) & 1   (the carry/borrow into bit 4; for
    //                                       logical ops AF is defined to be 0)
    // `pf_af_pending` says which of the two cached bytes are still authoritative:
    // when a bit is clear, the corresponding field of `flags` is definitive and
    // these bytes are stale. Nothing outside src/core/translator.cpp should read
    // them; interpreter code must call flush_pending_flags() first.
    std::uint8_t pf_af_pending = 0;
    std::uint8_t pf_af_res_low = 0; // result & 0xFF
    std::uint8_t pf_af_xor_low = 0; // (a ^ b ^ result) & 0xFF; bit 4 is AF

    double fpu_st[8] = {};
    XmmReg xmm[kXmmCount] = {};

    std::uint64_t& reg(Gpr r) { return gpr[static_cast<std::size_t>(r)]; }
    std::uint64_t reg(Gpr r) const { return gpr[static_cast<std::size_t>(r)]; }
};

std::string to_string(const GuestState& state);

// --- Pending PF/AF descriptor ----------------------------------------------
//
// `GuestState::pf_af_pending` bits: a set bit means "the corresponding field of
// `flags` has not been recomputed yet, use the cached bytes". The bit positions
// deliberately match the translator's internal flag-bit mask (bit 1 = PF, bit 2
// = AF), so generated code can use one value both to select the flags it writes
// and to record which ones are pending.
inline constexpr std::uint8_t kPfAfPendingPf = 1u << 1;
inline constexpr std::uint8_t kPfAfPendingAf = 1u << 2;
inline constexpr std::uint8_t kPfAfPendingBoth =
    static_cast<std::uint8_t>(kPfAfPendingPf | kPfAfPendingAf);

// Materialise any pending PF/AF into `state.flags` and clear the pending bits.
// Cheap and idempotent when nothing is pending; returns true if it did work.
// This is the C++ mirror of the generated-code epilogue in src/core/translator.cpp
// and must run before anything on the C++ side observes `state.flags`.
bool flush_pending_flags(GuestState& state);

// The descriptor lives in what used to be alignment padding, so adding it must
// not move any field that generated code hard-codes an offset for.
static_assert(offsetof(GuestState, fpu_st) == offsetof(GuestState, df) + 7,
              "pending PF/AF descriptor must fit the existing padding after `df`");
static_assert(offsetof(GuestState, pf_af_pending) == offsetof(GuestState, df) + 1,
              "pending PF/AF descriptor must start immediately after `df`");

const char* to_string(Gpr r);

} // namespace hyperion

#endif // HYPERION_GUEST_STATE_HPP
