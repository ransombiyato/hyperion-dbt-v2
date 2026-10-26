// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// P14 stage 1: per-instruction facts.
//
// `compute_ir_facts()` is the one place that knows what a decoded guest
// instruction does to registers, flags and memory. The register allocator and
// the flag-liveness pass already consume it; any later optimisation pass will.
// These tests pin the facts themselves, because a wrong fact here does not fail
// loudly - it silently makes an optimisation unsound.
//
// The cases were chosen for the distinctions that matter rather than for
// coverage: partial-register writes, high-byte aliases, INC/DEC leaving CF
// alone, MUL writing only CF/OF, conditions reading their own flag set, memory
// operands read-versus-written, and the implicit stack effects of
// push/pop/call/ret/leave. Bytes are encodings verified with as/objdump.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/decoder.hpp"
#include "hyperion/translator.hpp"

using hyperion::compute_ir_facts;
using hyperion::decode;
using hyperion::Gpr;
using hyperion::Instruction;
using hyperion::IrFacts;

namespace {

// Decode exactly one instruction from these bytes. Every case below is a single
// instruction, so a decoder surprise is a test failure rather than a cascade.
Instruction one(std::initializer_list<std::uint8_t> bytes) {
    const std::vector<std::uint8_t> v(bytes);
    const auto decoded = decode(v.data(), v.size(), 0x400000);
    HYPERION_CHECK(decoded.ok());
    return decoded.instruction;
}

constexpr std::uint16_t bit(Gpr r) {
    return static_cast<std::uint16_t>(1u << static_cast<unsigned>(r));
}

void check_gprs(const IrFacts& f, std::uint16_t reads, std::uint16_t writes) {
    if (f.gpr_reads != reads || f.gpr_writes != writes) {
        std::fprintf(stderr, "  gpr_reads=0x%04x (want 0x%04x) gpr_writes=0x%04x (want 0x%04x)\n",
                     f.gpr_reads, reads, f.gpr_writes, writes);
    }
    HYPERION_CHECK_EQ(f.gpr_reads, reads);
    HYPERION_CHECK_EQ(f.gpr_writes, writes);
}

void check_flags(const IrFacts& f, std::uint8_t read, std::uint8_t written, std::uint8_t killed) {
    if (f.flags_read != read || f.flags_written != written || f.flags_killed != killed) {
        std::fprintf(stderr, "  flags read=0x%02x written=0x%02x killed=0x%02x\n", f.flags_read,
                     f.flags_written, f.flags_killed);
    }
    HYPERION_CHECK_EQ(f.flags_read, read);
    HYPERION_CHECK_EQ(f.flags_written, written);
    HYPERION_CHECK_EQ(f.flags_killed, killed);
}

} // namespace

// --- ordinary moves and aliases ---------------------------------------------

HYPERION_TEST("ir_facts", "mov_reg_reg_reads_source_and_writes_destination") {
    // mov rax, rbx   48 89 D8
    const IrFacts f = compute_ir_facts(one({0x48, 0x89, 0xD8}));
    check_gprs(f, bit(Gpr::kRbx), bit(Gpr::kRax));
    check_flags(f, 0, 0, 0); // MOV never touches flags
    HYPERION_CHECK(!f.reads_memory);
    HYPERION_CHECK(!f.writes_memory);
    HYPERION_CHECK(!f.is_terminator);
    HYPERION_CHECK_EQ(f.step_weight, std::uint8_t{1});
}

HYPERION_TEST("ir_facts", "mov_immediate_reads_nothing") {
    // mov eax, 0x12345678   B8 78 56 34 12
    const IrFacts f = compute_ir_facts(one({0xB8, 0x78, 0x56, 0x34, 0x12}));
    check_gprs(f, 0, bit(Gpr::kRax));
    check_flags(f, 0, 0, 0);
}

HYPERION_TEST("ir_facts", "partial_register_write_marks_the_whole_register") {
    // mov al, bl   88 D8   -- a byte write leaves the upper bits of rax alone,
    // but the register's architectural value does change, so the emitter (which
    // reads-modifies-writes all 64 bits) must see rax as written.
    const IrFacts f = compute_ir_facts(one({0x88, 0xD8}));
    check_gprs(f, bit(Gpr::kRbx), bit(Gpr::kRax));
    // and the 16-bit form
    const IrFacts f16 = compute_ir_facts(one({0x66, 0x89, 0xD8})); // mov ax, bx
    check_gprs(f16, bit(Gpr::kRbx), bit(Gpr::kRax));
}

HYPERION_TEST("ir_facts", "high_byte_registers_alias_their_parent") {
    // test ah, bh   84 FC   -- AH/BH are bits 15..8 of RAX/RBX, so both registers
    // are read; this is the alias case a copy-propagation pass must not miss.
    const IrFacts f = compute_ir_facts(one({0x84, 0xFC}));
    HYPERION_CHECK((f.gpr_reads & bit(Gpr::kRax)) != 0);
    HYPERION_CHECK((f.gpr_reads & bit(Gpr::kRbx)) != 0);
    HYPERION_CHECK_EQ(f.gpr_writes, std::uint16_t{0});
}

// --- flags ------------------------------------------------------------------

HYPERION_TEST("ir_facts", "cmp_writes_every_flag_and_no_register") {
    // cmp rax, rbx   48 39 D8
    const IrFacts f = compute_ir_facts(one({0x48, 0x39, 0xD8}));
    check_gprs(f, bit(Gpr::kRax) | bit(Gpr::kRbx), 0);
    check_flags(f, 0, hyperion::ir_flags::kAll, hyperion::ir_flags::kAll);
}

HYPERION_TEST("ir_facts", "inc_and_dec_leave_the_carry_flag_alone") {
    // inc rax   48 FF C0
    const IrFacts f = compute_ir_facts(one({0x48, 0xFF, 0xC0}));
    check_gprs(f, bit(Gpr::kRax), bit(Gpr::kRax));
    const std::uint8_t no_cf =
        static_cast<std::uint8_t>(hyperion::ir_flags::kAll & ~hyperion::ir_flags::kCf);
    check_flags(f, 0, no_cf, no_cf);
    // dec rcx   48 FF C9
    const IrFacts d = compute_ir_facts(one({0x48, 0xFF, 0xC9}));
    check_flags(d, 0, no_cf, no_cf);
}

HYPERION_TEST("ir_facts", "mul_reports_its_flags_but_not_its_implicit_registers") {
    // mul rbx   48 F7 E3
    const IrFacts f = compute_ir_facts(one({0x48, 0xF7, 0xE3}));
    check_flags(f, 0, static_cast<std::uint8_t>(hyperion::ir_flags::kCf | hyperion::ir_flags::kOf),
                static_cast<std::uint8_t>(hyperion::ir_flags::kCf | hyperion::ir_flags::kOf));
    // Known limitation, pinned deliberately: the register-mask helpers this pass
    // is built on describe *explicit* operands (plus the stack pointer where the
    // allocator needs it) and do not model the implicit RAX:RDX pair of MUL, nor
    // DIV/IDIV/CQO, nor the implicit RSI/RDI/RCX of string operations. Those
    // opcodes are executed through the helper, which re-reads GuestState, so the
    // allocator is unaffected - but a future pass may not treat `gpr_writes` as
    // complete for them (audit §29.23). The assertions here record that boundary
    // so it cannot be forgotten silently.
    // Neither the explicit operand nor the implicit pair is reported today:
    // MUL/DIV/IDIV/CQO are absent from the register switch entirely and fall
    // through to `default: break`, so both masks are zero. Stage 1 must keep
    // this exactly as it is - adding the implicit registers here would change
    // the refcounts in pass 1.5 and therefore the generated code, breaking the
    // byte-identity requirement - so the boundary is pinned instead.
    HYPERION_CHECK_EQ(f.gpr_writes, std::uint16_t{0});
    HYPERION_CHECK_EQ(f.gpr_reads, std::uint16_t{0});
}

HYPERION_TEST("ir_facts", "adc_reads_the_carry_it_consumes") {
    // adc rax, rbx   48 11 D8
    const IrFacts f = compute_ir_facts(one({0x48, 0x11, 0xD8}));
    HYPERION_CHECK_EQ(f.flags_read, hyperion::ir_flags::kCf);
    check_flags(f, hyperion::ir_flags::kCf, hyperion::ir_flags::kAll, hyperion::ir_flags::kAll);
}

HYPERION_TEST("ir_facts", "conditions_read_exactly_their_own_flags") {
    // je rel8   74 00   -- ZF only
    const IrFacts je = compute_ir_facts(one({0x74, 0x00}));
    HYPERION_CHECK_EQ(je.flags_read, hyperion::ir_flags::kZf);
    HYPERION_CHECK(je.is_terminator);
    // jb rel8   72 00   -- CF only
    HYPERION_CHECK_EQ(compute_ir_facts(one({0x72, 0x00})).flags_read, hyperion::ir_flags::kCf);
    // jbe rel8  76 00   -- CF or ZF
    HYPERION_CHECK_EQ(compute_ir_facts(one({0x76, 0x00})).flags_read,
                      static_cast<std::uint8_t>(hyperion::ir_flags::kCf | hyperion::ir_flags::kZf));
    // seto al   0F 90 C0 -- OF, and not a terminator
    const IrFacts seto = compute_ir_facts(one({0x0F, 0x90, 0xC0}));
    HYPERION_CHECK_EQ(seto.flags_read, hyperion::ir_flags::kOf);
    HYPERION_CHECK(!seto.is_terminator);
}

// --- memory -----------------------------------------------------------------

HYPERION_TEST("ir_facts", "a_store_reads_the_address_and_writes_memory") {
    // mov [rbx], eax   89 03
    const IrFacts f = compute_ir_facts(one({0x89, 0x03}));
    HYPERION_CHECK(f.reads_memory); // the addressing form itself can fault
    HYPERION_CHECK(f.writes_memory);
    HYPERION_CHECK((f.gpr_reads & bit(Gpr::kRbx)) != 0); // base register
    HYPERION_CHECK((f.gpr_reads & bit(Gpr::kRax)) != 0); // stored value
}

HYPERION_TEST("ir_facts", "a_compare_against_memory_reads_it_and_writes_nothing") {
    // cmp [rbx], eax   39 03
    const IrFacts f = compute_ir_facts(one({0x39, 0x03}));
    HYPERION_CHECK(f.reads_memory);
    HYPERION_CHECK(!f.writes_memory); // CMP must never be treated as a store
    // test [rbx], al   84 03
    const IrFacts t = compute_ir_facts(one({0x84, 0x03}));
    HYPERION_CHECK(t.reads_memory);
    HYPERION_CHECK(!t.writes_memory);
}

HYPERION_TEST("ir_facts", "index_registers_count_as_reads") {
    // mov rax, [rbx + rcx * 4]   48 8B 04 8B
    const IrFacts f = compute_ir_facts(one({0x48, 0x8B, 0x04, 0x8B}));
    HYPERION_CHECK((f.gpr_reads & bit(Gpr::kRbx)) != 0);
    HYPERION_CHECK((f.gpr_reads & bit(Gpr::kRcx)) != 0);
    HYPERION_CHECK((f.gpr_writes & bit(Gpr::kRax)) != 0);
    HYPERION_CHECK(f.reads_memory);
    HYPERION_CHECK(!f.writes_memory);
}

// --- control flow and the implicit stack ------------------------------------

HYPERION_TEST("ir_facts", "stack_operations_report_their_implicit_accesses") {
    // push rax   50  -- writes the guest stack, touches rsp
    const IrFacts p = compute_ir_facts(one({0x50}));
    HYPERION_CHECK(p.writes_memory);
    HYPERION_CHECK((p.gpr_writes & bit(Gpr::kRsp)) != 0);
    // pop rax    58  -- reads the guest stack
    const IrFacts q = compute_ir_facts(one({0x58}));
    HYPERION_CHECK(q.reads_memory);
    HYPERION_CHECK((q.gpr_writes & bit(Gpr::kRsp)) != 0);
    // call rel32  E8 00 00 00 00 -- pushes a return address, ends the block
    const IrFacts c = compute_ir_facts(one({0xE8, 0x00, 0x00, 0x00, 0x00}));
    HYPERION_CHECK(c.writes_memory);
    HYPERION_CHECK(c.is_terminator);
    // ret         C3 -- pops it
    const IrFacts r = compute_ir_facts(one({0xC3}));
    HYPERION_CHECK(r.reads_memory);
    HYPERION_CHECK(r.is_terminator);
    // leave        C9 -- reads and writes the stack, restores rbp
    const IrFacts l = compute_ir_facts(one({0xC9}));
    HYPERION_CHECK(l.reads_memory);
    HYPERION_CHECK(l.writes_memory);
    HYPERION_CHECK((l.gpr_writes & bit(Gpr::kRbp)) != 0);
}

HYPERION_TEST("ir_facts", "terminators_are_flagged_and_fallthrough_is_not") {
    // jmp rel8   EB 00
    HYPERION_CHECK(compute_ir_facts(one({0xEB, 0x00})).is_terminator);
    // hlt        F4
    HYPERION_CHECK(compute_ir_facts(one({0xF4})).is_terminator);
    // jmp rbx    FF E3
    HYPERION_CHECK(compute_ir_facts(one({0xFF, 0xE3})).is_terminator);
    // nop        90
    HYPERION_CHECK(!compute_ir_facts(one({0x90})).is_terminator);
    // add rax, rbx  48 01 D8
    HYPERION_CHECK(!compute_ir_facts(one({0x48, 0x01, 0xD8})).is_terminator);
}

HYPERION_TEST("ir_facts", "string_operations_are_conservative_about_memory") {
    // rep movsb   F3 A4 -- direction depends on DF at run time, and CMPS/SCAS
    // only read while MOVS/STOS write, so both bits must be set.
    const IrFacts f = compute_ir_facts(one({0xF3, 0xA4}));
    HYPERION_CHECK(f.reads_memory);
    HYPERION_CHECK(f.writes_memory);
}

// --- purity / invalidation --------------------------------------------------

HYPERION_TEST("ir_facts", "facts_are_pure_and_reproducible") {
    // The facts are a pure function of the decoded instruction: computing them
    // twice, or from two independent decodes of the same bytes, must agree. This
    // is the property that lets them be computed once per block and cached in the
    // IrInsn without any invalidation machinery (audit §29.23).
    const std::vector<std::uint8_t> variants[] = {
        {0x48, 0x01, 0xD8},             // add rax, rbx
        {0x83, 0xF8, 0x14},             // cmp eax, 20
        {0x0F, 0x94, 0xC0},             // sete al
        {0x48, 0x8B, 0x04, 0x8B},       // mov rax, [rbx + rcx*4]
        {0xE8, 0x00, 0x00, 0x00, 0x00}, // call
    };
    for (const auto& bytes : variants) {
        const auto d1 = decode(bytes.data(), bytes.size(), 0x400000);
        const auto d2 = decode(bytes.data(), bytes.size(), 0x400000);
        HYPERION_CHECK(d1.ok());
        HYPERION_CHECK(d2.ok());
        const IrFacts a = compute_ir_facts(d1.instruction);
        const IrFacts b = compute_ir_facts(d1.instruction);
        const IrFacts c = compute_ir_facts(d2.instruction);
        HYPERION_CHECK_EQ(a.flags_read, b.flags_read);
        HYPERION_CHECK_EQ(a.flags_written, c.flags_written);
        HYPERION_CHECK_EQ(a.flags_killed, c.flags_killed);
        HYPERION_CHECK_EQ(a.gpr_reads, b.gpr_reads);
        HYPERION_CHECK_EQ(a.gpr_writes, c.gpr_writes);
        HYPERION_CHECK_EQ(a.reads_memory, c.reads_memory);
        HYPERION_CHECK_EQ(a.writes_memory, c.writes_memory);
        HYPERION_CHECK_EQ(a.is_terminator, c.is_terminator);
    }
}

HYPERION_TEST("ir_facts", "flags_written_is_never_narrower_than_flags_killed") {
    // Anything definitely overwritten is certainly written. A pass may rely on
    // this even though the two are equal for every opcode today.
    const std::vector<std::uint8_t> corpus[] = {
        {0x48, 0x01, 0xD8}, // add
        {0x48, 0x29, 0xD8}, // sub
        {0x48, 0x39, 0xD8}, // cmp
        {0x48, 0x85, 0xD8}, // test
        {0x48, 0x21, 0xD8}, // and
        {0x48, 0xFF, 0xC0}, // inc
        {0x48, 0xFF, 0xC9}, // dec
        {0x48, 0xF7, 0xE3}, // mul
        {0x48, 0xD1, 0xE0}, // shl rax, 1
        {0x0F, 0x94, 0xC0}, // sete
    };
    for (const auto& bytes : corpus) {
        const auto d = decode(bytes.data(), bytes.size(), 0x400000);
        HYPERION_CHECK(d.ok());
        const IrFacts f = compute_ir_facts(d.instruction);
        if ((f.flags_killed & ~f.flags_written) != 0) {
            std::fprintf(stderr, "  opcode %u killed=0x%02x written=0x%02x\n",
                         static_cast<unsigned>(d.instruction.opcode), f.flags_killed,
                         f.flags_written);
        }
        HYPERION_CHECK_EQ(static_cast<std::uint8_t>(f.flags_killed & ~f.flags_written),
                          std::uint8_t{0});
    }
}
