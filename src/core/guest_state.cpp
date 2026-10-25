// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "hyperion/guest_state.hpp"

#include <array>
#include <cstdio>

namespace hyperion {

namespace {

constexpr std::array<const char*, kGprCount> kGprNames = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
};

} // namespace

const char* to_string(Gpr r) {
    const auto index = static_cast<std::size_t>(r);
    return index < kGprCount ? kGprNames[index] : "???";
}

std::string to_string(const Flags& flags) {
    std::string out;
    out += flags.cf ? 'C' : '-';
    out += flags.pf ? 'P' : '-';
    out += flags.af ? 'A' : '-';
    out += flags.zf ? 'Z' : '-';
    out += flags.sf ? 'S' : '-';
    out += flags.of ? 'O' : '-';
    return out;
}

std::uint64_t pack_flags(const Flags& flags) {
    std::uint64_t value = kRflagsBitReserved1; // bit 1 is architecturally set
    if (flags.cf) {
        value |= kRflagsBitCf;
    }
    if (flags.pf) {
        value |= kRflagsBitPf;
    }
    if (flags.af) {
        value |= kRflagsBitAf;
    }
    if (flags.zf) {
        value |= kRflagsBitZf;
    }
    if (flags.sf) {
        value |= kRflagsBitSf;
    }
    if (flags.of) {
        value |= kRflagsBitOf;
    }
    return value;
}

Flags unpack_flags(std::uint64_t rflags) {
    Flags flags;
    flags.cf = (rflags & kRflagsBitCf) != 0;
    flags.pf = (rflags & kRflagsBitPf) != 0;
    flags.af = (rflags & kRflagsBitAf) != 0;
    flags.zf = (rflags & kRflagsBitZf) != 0;
    flags.sf = (rflags & kRflagsBitSf) != 0;
    flags.of = (rflags & kRflagsBitOf) != 0;
    return flags;
}

bool flush_pending_flags(GuestState& state) {
    const std::uint8_t pending = state.pf_af_pending;
    if (pending == 0) {
        return false;
    }
    if ((pending & kPfAfPendingPf) != 0) {
        // x86 PF is set when the low byte has an even number of set bits.
        state.flags.pf = __builtin_parity(static_cast<unsigned>(state.pf_af_res_low)) == 0;
    }
    if ((pending & kPfAfPendingAf) != 0) {
        state.flags.af = ((state.pf_af_xor_low >> 4) & 1u) != 0;
    }
    state.pf_af_pending = static_cast<std::uint8_t>(pending & ~kPfAfPendingBoth);
    return true;
}

std::string to_string(const GuestState& state) {
    std::string out;
    char line[64];
    for (std::size_t i = 0; i < kGprCount; ++i) {
        std::snprintf(line, sizeof(line), "%-4s=0x%016llx", kGprNames[i],
                      static_cast<unsigned long long>(state.gpr[i]));
        out += line;
        out += (i % 4 == 3) ? '\n' : ' ';
    }
    std::snprintf(line, sizeof(line), "rip =0x%016llx", static_cast<unsigned long long>(state.rip));
    out += line;
    out += "\nflags=";
    out += to_string(state.flags);
    out += " (";
    std::snprintf(line, sizeof(line), "0x%llx",
                  static_cast<unsigned long long>(pack_flags(state.flags)));
    out += line;
    out += ")\n";
    return out;
}

} // namespace hyperion
