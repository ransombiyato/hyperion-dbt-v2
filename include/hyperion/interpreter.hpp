// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Reference interpreter and shared software-TLB contract.
#ifndef HYPERION_INTERPRETER_HPP
#define HYPERION_INTERPRETER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "hyperion/decoder.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/run.hpp"

namespace hyperion {

// Result of resolving one 4 KiB guest page (`page_addr` aligned to 4096 bytes)
// for software-TLB acceleration. Implementations that cannot expose contiguous
// host storage (or mock fault-injection `Memory` subclasses in tests) leave
// `host_page = nullptr`, cleanly forcing all accesses through virtual read/write.
struct PageAccess {
    std::uint8_t* host_page = nullptr;
    bool can_read = false;
    bool can_write_fast = false; // writable AND NOT executable (preserves SMC coherence)
};

// Direct-mapped software-TLB entry shared by Tier 0 and Tier 1 inline codegen.
// Invariant: for slot `k = (addr >> 12) & (kSoftTlbEntries - 1)`:
//   - Valid entry has `page_tag == (addr & ~0xFFFULL)` and
//     `host_addend == uintptr_t(host_page) - page_tag` (mod 2^64).
//   - Invalid/empty entry has `page_tag == empty_soft_tlb_tag(k)`, whose bit 12
//     differs from `k & 1` and bits 11..0 are 0, guaranteeing unsigned
//     `(addr - page_tag) >= 4096` across all 2^64 guest addresses with zero false hits.
inline constexpr std::size_t kSoftTlbBits = 9;
inline constexpr std::size_t kSoftTlbEntries = std::size_t{1} << kSoftTlbBits; // 512 pages (2 MiB)
inline constexpr std::size_t kSoftTlbMask = kSoftTlbEntries - 1;

struct SoftTlbEntry {
    std::uint64_t page_tag = 0;
    std::uint64_t host_addend = 0;
};
static_assert(sizeof(SoftTlbEntry) == 16, "Tier 1 inline AArch64 SoftTLB uses lsl #4 indexing");

constexpr std::uint64_t empty_soft_tlb_tag(std::size_t slot) noexcept {
    return ((static_cast<std::uint64_t>(slot) ^ 1ULL) << 12) | (1ULL << 63);
}

struct SoftTlb {
    std::array<SoftTlbEntry, kSoftTlbEntries> read_entries{};
    std::array<SoftTlbEntry, kSoftTlbEntries> write_entries{};
    std::uint64_t epoch = 0;

    SoftTlb() noexcept { flush(0); }

    void flush(std::uint64_t new_epoch) noexcept {
        for (std::size_t i = 0; i < kSoftTlbEntries; ++i) {
            const std::uint64_t tag = empty_soft_tlb_tag(i);
            read_entries[i] = SoftTlbEntry{tag, 0};
            write_entries[i] = SoftTlbEntry{tag, 0};
        }
        epoch = new_epoch;
    }
};

// Abstract guest memory.
class Memory {
public:
    virtual ~Memory() = default;
    virtual bool read(std::uint64_t address, std::uint8_t* out, std::size_t count) = 0;
    virtual bool write(std::uint64_t address, const std::uint8_t* in, std::size_t count) = 0;
    virtual bool fetch(std::uint64_t address, std::uint8_t* out, std::size_t count) {
        return read(address, out, count);
    }
    virtual std::uint64_t code_epoch() const noexcept { return 0; }

    // Generation counter for *mapping* changes: any operation that can add,
    // remove, resize, move or re-permission a guest page bumps this, and a
    // SoftTlb entry derived from a page is invalid once it changes.
    //
    // This is deliberately not the same counter as code_epoch(). code_epoch()
    // tracks self-modifying *code* (and so drives translation invalidation), and
    // address spaces legitimately bump it only when executable memory is
    // involved. Reusing it to decide "do my cached host page pointers still hold"
    // was wrong: an ordinary `mprotect(PROT_READ|PROT_WRITE)` (RELRO) or a plain
    // non-exec `mmap`/`munmap` moves backing stores without touching code, and the
    // data SoftTlb then pointed at freed memory. The default here returns
    // code_epoch() so any implementation that does not distinguish the two stays
    // conservatively correct (it will simply flush more often).
    virtual std::uint64_t mapping_epoch() const noexcept { return code_epoch(); }
    virtual PageAccess translate_page(std::uint64_t page_addr) noexcept {
        (void)page_addr;
        return {};
    }
};

// A flat, byte-addressable region [base, base + size).
class FlatMemory final : public Memory {
public:
    FlatMemory(std::uint64_t base, std::size_t size);

    bool read(std::uint64_t address, std::uint8_t* out, std::size_t count) override;
    bool write(std::uint64_t address, const std::uint8_t* in, std::size_t count) override;
    std::uint64_t code_epoch() const noexcept override { return epoch_; }
    PageAccess translate_page(std::uint64_t page_addr) noexcept override;

    std::uint64_t base() const { return base_; }
    std::size_t size() const { return bytes_.size(); }

    std::uint8_t* data() { return bytes_.data(); }
    const std::uint8_t* data() const { return bytes_.data(); }

private:
    bool contains(std::uint64_t address, std::size_t count) const;

    std::uint64_t base_;
    std::vector<std::uint8_t> bytes_;
    std::uint64_t epoch_ = 1;
};

enum class StepStatus : std::uint8_t {
    kOk,          // one instruction executed; state updated
    kHalted,      // HLT/top-level RET executed, or exit syscall reached
    kFault,       // memory fault, divide fault, or invalid state
    kUnsupported, // decode failed: instruction is outside the subset
};

struct StepResult {
    StepStatus status = StepStatus::kOk;
    DecodeError decode_error = DecodeError::kNone;
    Instruction instruction{};
    std::string message;
    std::uint32_t unimplemented_syscalls = 0;
    bool exited_via_syscall = false;
    std::int64_t exit_code = 0;

    bool ok() const { return status == StepStatus::kOk; }
};

class SyscallHandler;

StepResult step(GuestState& state, Memory& memory, SyscallHandler* syscalls = nullptr);

namespace alu {

std::uint64_t add(std::uint64_t a, std::uint64_t b, int width, Flags& flags);
std::uint64_t adc(std::uint64_t a, std::uint64_t b, bool cf_in, int width, Flags& flags);
std::uint64_t sub(std::uint64_t a, std::uint64_t b, int width, Flags& flags);
std::uint64_t sbb(std::uint64_t a, std::uint64_t b, bool cf_in, int width, Flags& flags);

std::uint64_t logical_and(std::uint64_t a, std::uint64_t b, int width, Flags& flags);
std::uint64_t logical_or(std::uint64_t a, std::uint64_t b, int width, Flags& flags);
std::uint64_t logical_xor(std::uint64_t a, std::uint64_t b, int width, Flags& flags);

std::uint64_t inc(std::uint64_t a, int width, Flags& flags);
std::uint64_t dec(std::uint64_t a, int width, Flags& flags);

bool evaluate(Condition condition, const Flags& flags);
void set_szp(std::uint64_t result, int width, Flags& flags);

} // namespace alu

// Execute a packed SSE2 operation (`insn.opcode == Opcode::kSse`) against `state`
// and `memory` (using `tlb` when non-null). Shared by Tier 0 and Tier 1 so complex
// SIMD lane semantics exist in one exact implementation. Returns false on memory fault.
bool execute_sse_instruction(const Instruction& insn, GuestState& state, Memory& memory,
                             SoftTlb* tlb = nullptr);

// Execute a `REP` / non-`REP` string instruction (`Opcode::kStringOp`) against
// `state` and `memory`. Shared by Tier 0 and Tier 1. Returns false on memory fault.
bool execute_string_instruction(const Instruction& insn, GuestState& state, Memory& memory,
                                SoftTlb* tlb = nullptr);

// Populate baseline x86-64 + SSE2 CPUID result in `state` for `EAX`/`ECX` inputs.
void execute_cpuid(GuestState& state) noexcept;

bool soft_tlb_read_slow(Memory& memory, SoftTlb& tlb, std::uint64_t addr, void* out,
                        std::size_t bytes) noexcept;
bool soft_tlb_write_slow(Memory& memory, SoftTlb& tlb, std::uint64_t addr, const void* in,
                         std::size_t bytes) noexcept;
bool execute_cached_helper(const Instruction& insn, GuestState& state, Memory& memory,
                           SoftTlb* tlb = nullptr);

RunResult run_program(Memory& memory, GuestState state, std::uint64_t max_steps = 1'000'000,
                      SyscallHandler* syscalls = nullptr);

} // namespace hyperion

#endif // HYPERION_INTERPRETER_HPP
