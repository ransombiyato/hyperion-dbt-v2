// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// x86-64 user-mode instruction decoder.
//
// Design (see DECISIONS.md D-014 and COMPATIBILITY.md):
//   - Decoding is explicit and total: a byte sequence either decodes to a
//     known instruction with exact architectural byte length and operand
//     widths, or is rejected with an explicit DecodeError reason.
//   - Operands are decoded into a uniform representation (GPR, XMM, immediate,
//     or memory address with optional FS: segment base and 32/64-bit address
//     width) consumed identically by Tier 0 and Tier 1.
#ifndef HYPERION_DECODER_HPP
#define HYPERION_DECODER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hyperion/guest_state.hpp"

namespace hyperion {

// Why a byte sequence was rejected. Kept explicit so tests can assert the
// precise reason rather than "it failed".
enum class DecodeError : std::uint8_t {
    kNone = 0,
    kTruncated,          // ran out of bytes mid-instruction
    kUnknownOpcode,      // opcode not in the supported subset
    kUnsupportedPrefix,  // prefix outside the modelled set (e.g. GS:, invalid LOCK)
    kUnsupportedOperand, // operand form outside the subset
    kUnsupportedModrm,   // ModRM/SIB combination not valid (e.g. LEA with mod=3)
    kTooLong,            // exceeded the 15-byte architectural limit
};

const char* to_string(DecodeError error);

// Architectural operand width in bytes.
enum class OperandSize : std::uint8_t {
    k8 = 1,
    k16 = 2,
    k32 = 4,
    k64 = 8,
    k128 = 16,
};

// How an operand is encoded by an instruction.
enum class OperandKind : std::uint8_t {
    kRegister,
    kImmediate,
    kMemory,
    kXmm,
};

// A decoded memory operand: [fs_base +] (rip | base) + index*scale + disp.
struct MemoryOperand {
    bool rip_relative = false;
    bool fs_relative = false;
    bool has_base = false;
    Gpr base = Gpr::kRax;
    bool has_index = false;
    Gpr index = Gpr::kRax;
    std::uint8_t scale = 1;
    std::int64_t disp = 0;
    OperandSize address_size = OperandSize::k64;
};

struct Operand {
    OperandKind kind = OperandKind::kRegister;
    OperandSize size = OperandSize::k64;
    Gpr reg = Gpr::kRax;    // when kind == kRegister
    std::uint8_t xmm = 0;   // when kind == kXmm (0..15)
    bool high_byte = false; // AH/CH/DH/BH when kind == kRegister && size == k8
    std::uint64_t imm = 0;  // when kind == kImmediate
    MemoryOperand mem;      // when kind == kMemory
};

// Sub-operation selector for SSE2 instructions (`Opcode::kSse`).
enum class SseOp : std::uint8_t {
    kNone = 0,
    kMovUps,     // 128-bit load/store/reg move (0F 10/11, 0F 28/29, 66 0F 6F/7F, F3 0F 6F/7F)
    kMovQ,       // 64-bit load/store/move (66 0F 6E/7E, F3 0F 7E, 66 0F D6)
    kMovD,       // 32-bit GPR/mem <-> XMM (66 0F 6E/7E without REX.W)
    kMovLpd,     // 64-bit low lane load/store preserving upper 64b (0F 12/13, 66 0F 12/13)
    kMovHpd,     // 64-bit high lane load/store preserving lower 64b (0F 16/17, 66 0F 16/17)
    kMovLhps,    // 0F 16 mod=3: dst.hi = src.lo
    kMovHlps,    // 0F 12 mod=3: dst.lo = src.hi
    kPxor,       // 66 0F EF / 0F 57
    kPor,        // 66 0F EB / 0F 56
    kPand,       // 66 0F DB / 0F 54
    kPandn,      // 66 0F DF / 0F 55
    kPcmpeqb,    // 66 0F 74
    kPcmpeqw,    // 66 0F 75
    kPcmpeqd,    // 66 0F 76
    kPcmpgtb,    // 66 0F 64
    kPcmpgtw,    // 66 0F 65
    kPcmpgtd,    // 66 0F 66
    kPmovmskb,   // 66 0F D7: r32/64 = byte sign mask of xmm
    kPunpcklbw,  // 66 0F 60
    kPunpcklwd,  // 66 0F 61
    kPunpckldq,  // 66 0F 62
    kPunpcklqdq, // 66 0F 6C
    kPunpckhbw,  // 66 0F 68
    kPunpckhwd,  // 66 0F 69
    kPunpckhdq,  // 66 0F 6A
    kPunpckhqdq, // 66 0F 6D
    kPackuswb,   // 66 0F 67
    kPacksswb,   // 66 0F 63
    kPackssdw,   // 66 0F 6B
    kPshufd,     // 66 0F 70 imm8
    kPshufhw,    // F3 0F 70 imm8
    kPshuflw,    // F2 0F 70 imm8
    kPsrlwImm,   // 66 0F 71 /2 imm8
    kPsrawImm,   // 66 0F 71 /4 imm8
    kPsllwImm,   // 66 0F 71 /6 imm8
    kPsrldImm,   // 66 0F 72 /2 imm8
    kPsradImm,   // 66 0F 72 /4 imm8
    kPslldImm,   // 66 0F 72 /6 imm8
    kPsrlqImm,   // 66 0F 73 /2 imm8
    kPsrldqImm,  // 66 0F 73 /3 imm8 (byte shift right)
    kPsllqImm,   // 66 0F 73 /6 imm8
    kPslldqImm,  // 66 0F 73 /7 imm8 (byte shift left)
    kPaddb,      // 66 0F FC
    kPaddw,      // 66 0F FD
    kPaddd,      // 66 0F FE
    kPaddq,      // 66 0F D4
    kPsubb,      // 66 0F F8
    kPsubw,      // 66 0F F9
    kPsubd,      // 66 0F FA
    kPsubq,      // 66 0F FB
    kPminub,     // 66 0F DA
    kPmaxub,     // 66 0F DE
    kFxsave,     // 0F AE /0 (512-byte FXSAVE / XSAVE)
    kFxrstor,    // 0F AE /1 (512-byte FXRSTOR / XRSTOR)
    kLdmxcsr,    // 0F AE /2 (32-bit LDMXCSR)
    kStmxcsr,    // 0F AE /3 (32-bit STMXCSR)
    kXgetbv,     // 0F 01 D0 (XGETBV -> EAX=3, EDX=0)
    kRdrand,     // 0F C7 /6 | /7 (RDRAND / RDSEED)
    kMovSsReg,   // F3 0F 10 mod=3 (dst[31:0] = src[31:0], dst[127:32] preserved)
    kAddPs,
    kAddPd,
    kAddSs,
    kAddSd,
    kSubPs,
    kSubPd,
    kSubSs,
    kSubSd,
    kMulPs,
    kMulPd,
    kMulSs,
    kMulSd,
    kDivPs,
    kDivPd,
    kDivSs,
    kDivSd,
    kSqrtPs,
    kSqrtPd,
    kSqrtSs,
    kSqrtSd,
    kMinPs,
    kMinPd,
    kMinSs,
    kMinSd,
    kMaxPs,
    kMaxPd,
    kMaxSs,
    kMaxSd,
    kUcomiss,
    kUcomisd,
    kCmpPs,
    kCmpPd,
    kCmpSs,
    kCmpSd,
    kShufPs,
    kShufPd,
    kMovmskPs,
    kMovmskPd,
    kCvtsi2ss,
    kCvtsi2sd,
    kCvttss2si,
    kCvttsd2si,
    kCvtss2si,
    kCvtsd2si,
    kCvtss2sd,
    kCvtsd2ss,
    kCvtps2pd,
    kCvtpd2ps,
    kCvtdq2ps,
    kCvtps2dq,
    kCvttps2dq,
    kCvtdq2pd,
    kCvttpd2dq,
    kCvtpd2dq,
    kX87,
};

// The supported instruction mnemonics.
enum class Opcode : std::uint8_t {
    kMov,      // MOV r/m, r | MOV r, r/m | MOV r/m, imm | MOV r, imm
    kMovzx,    // MOVZX r16/32/64, r/m8 | r/m16
    kMovsx,    // MOVSX / MOVSXD r16/32/64, r/m8 | r/m16 | r/m32
    kLea,      // LEA r, m
    kAdd,      // ADD
    kAdc,      // ADC
    kSub,      // SUB
    kSbb,      // SBB
    kCmp,      // CMP
    kTest,     // TEST
    kAnd,      // AND
    kOr,       // OR
    kXor,      // XOR
    kInc,      // INC r/m
    kDec,      // DEC r/m
    kNot,      // NOT r/m
    kNeg,      // NEG r/m
    kShl,      // SHL / SAL r/m, 1 | cl | imm8
    kShr,      // SHR r/m, 1 | cl | imm8
    kSar,      // SAR r/m, 1 | cl | imm8
    kRol,      // ROL r/m, 1 | cl | imm8
    kRor,      // ROR r/m, 1 | cl | imm8
    kShld,     // SHLD r/m, r, imm8 | cl
    kShrd,     // SHRD r/m, r, imm8 | cl
    kImul,     // IMUL (1-op, 2-op, 3-op)
    kMul,      // MUL r/m (unsigned widening)
    kDiv,      // DIV r/m (unsigned)
    kIdiv,     // IDIV r/m (signed)
    kCdqe,     // CBW / CWDE / CDQE (sign-extend AL/AX/EAX -> AX/EAX/RAX)
    kCqo,      // CWD / CDQ / CQO (sign-extend AX/EAX/RAX -> DX:AX / EDX:EAX / RDX:RAX)
    kSetcc,    // SETcc r/m8
    kCmovcc,   // CMOVcc r16/32/64, r/m16/32/64
    kXchg,     // XCHG
    kCmpxchg,  // CMPXCHG r/m, r
    kXadd,     // XADD r/m, r
    kBswap,    // BSWAP r32/r64
    kBsf,      // BSF / TZCNT (rep == 0xF3 -> TZCNT)
    kBsr,      // BSR / LZCNT (rep == 0xF3 -> LZCNT)
    kBt,       // BT r/m, r | imm8
    kBts,      // BTS r/m, r | imm8
    kBtr,      // BTR r/m, r | imm8
    kBtc,      // BTC r/m, r | imm8
    kNop,      // NOP / multi-byte NOP / ENDBR64
    kCall,     // CALL rel32 | r/m64
    kRet,      // RET [imm16]
    kLeave,    // LEAVE (mov rsp, rbp ; pop rbp)
    kHlt,      // HLT
    kJmp,      // JMP rel8 | rel32 | r/m64
    kJcc,      // Jcc rel8 | rel32
    kPush,     // PUSH r64 | r/m64 | imm8 | imm32
    kPop,      // POP r64 | r/m64
    kStringOp, // MOVS / STOS / CMPS / SCAS (with optional REP / REPE / REPNE)
    kCld,      // CLD / STD
    kCpuid,    // CPUID (0F A2)
    kRdtsc,    // RDTSC (0F 31)
    kSse,      // Baseline SSE2 operations (sub-selected via `sse_op`)
    kSyscall,  // SYSCALL (0F 05)
};

const char* to_string(Opcode opcode);

// Condition codes for Jcc / SETcc / CMOVcc (low nibble 0x0..0xF).
enum class Condition : std::uint8_t {
    kO = 0x0,
    kNo = 0x1,
    kB = 0x2,
    kAe = 0x3,
    kE = 0x4,
    kNe = 0x5,
    kBe = 0x6,
    kA = 0x7,
    kS = 0x8,
    kNs = 0x9,
    kP = 0xA,
    kNp = 0xB,
    kL = 0xC,
    kGe = 0xD,
    kLe = 0xE,
    kG = 0xF,
};

const char* to_string(Condition condition);

// A fully decoded instruction. `length` is the number of bytes consumed, so
// the caller advances RIP by exactly that much.
struct Instruction {
    Opcode opcode = Opcode::kNop;
    SseOp sse_op = SseOp::kNone;
    Condition condition = Condition::kE; // valid for kJcc / kSetcc / kCmovcc
    std::uint64_t address = 0;           // address of the first byte
    std::uint8_t length = 0;             // bytes consumed
    Operand dst;                         // destination / first operand
    Operand src;                         // source / second operand
    Operand aux;                         // third operand (3-op IMUL, SHLD/SHRD, PSHUFD)
    bool has_src = false;
    bool has_aux = false;
    bool rex_w = false; // 64-bit operand size was selected
    // 0xF0 LOCK prefix, validated on a memory read-modify-write.
    //
    // It is recorded, not acted on, and the reason is an invariant rather than an
    // omission: Hyperion refuses thread creation (CLONE_THREAD / CLONE_VM /
    // CLONE_SIGHAND all return -ENOSYS in process.cpp), and each guest process is
    // a single host thread. A LOCK-prefixed RMW in a single-threaded process is
    // observationally identical to the plain RMW - there is no second observer to
    // see an intermediate value, and no peer whose stores need ordering - so
    // executing it as a plain RMW is *correct today*, not merely tolerated.
    //
    // The moment guest threads exist this becomes wrong, which is why the flag is
    // carried in the decoded instruction: an implementation of threads must either
    // emit an atomics sequence (LDXR/STXR or LSE LDADD) here or refuse the prefix.
    // Half of that contract is pinned by
    // tests/unit/test_decoder.cpp::decoder.lock_prefix_is_recorded_and_does_not_
    // change_semantics (the prefix is recorded and changes nothing); the other
    // half - that thread creation is refused - is pinned by the process tests.
    bool lock = false;
    std::uint8_t rep = 0;   // 0, 0xF2 (REPNE), or 0xF3 (REP/REPE)
    std::uint8_t subop = 0; // string op base opcode (A4..AF) or CLD(0)/STD(1)
};

struct DecodeResult {
    DecodeError error = DecodeError::kNone;
    Instruction instruction{};

    bool ok() const { return error == DecodeError::kNone; }
    explicit operator bool() const { return ok(); }
};

// Decode one instruction from `code` (at most `size` bytes) located at guest
// address `address`. Never reads past `size` bytes.
DecodeResult decode(const std::uint8_t* code, std::size_t size, std::uint64_t address);

// Human-readable form, used in test failure messages and diagnostics.
std::string to_string(const Instruction& instruction);

} // namespace hyperion

#endif // HYPERION_DECODER_HPP
