// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Tier 1 dispatcher: manages `ExecArena`, shared AArch64 entry/dispatch/exit
// trampolines, `ExecContext` (`SoftTlb` + direct-mapped `jump_table`), and
// multi-block native execution.
#include "hyperion/translation_cache.hpp"

#include <algorithm>
#include <chrono>
#include <string>

namespace hyperion {

namespace {

using EnterJitFn = RawBlockReturn (*)(GuestState*, std::uint64_t, ExecContext*, void*, void*);

constexpr std::size_t kMaxWindow = 4096;

std::size_t read_window(Memory& memory, std::uint64_t pc, std::uint8_t* out, std::size_t capacity) {
    for (std::size_t want = capacity; want >= 1; want /= 2) {
        if (memory.fetch(pc, out, want)) {
            return want;
        }
        if (want == 1) {
            break;
        }
    }
    return 0;
}

bool jit_mem_read_slow(ExecContext* ctx, std::uint64_t addr, void* out,
                       std::size_t bytes) noexcept {
    return soft_tlb_read_slow(*ctx->memory, ctx->tlb, addr, out, bytes);
}

bool jit_mem_write_slow(ExecContext* ctx, std::uint64_t addr, const void* in,
                        std::size_t bytes) noexcept {
    return soft_tlb_write_slow(*ctx->memory, ctx->tlb, addr, in, bytes);
}

// Accumulates translation wall time on every exit path from translate_at(),
// including the failure paths: a translation that failed still cost what it
// cost, and hiding that is how a slow cold start goes unexplained.
struct TranslateTimer {
    std::uint64_t& sink;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~TranslateTimer() {
        sink += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - start)
                                               .count());
    }
};

bool jit_exec_complex(ExecContext* ctx, GuestState* state, const Instruction* insn) noexcept {
    return execute_cached_helper(*insn, *state, *ctx->memory, &ctx->tlb);
}

} // namespace

TranslationCache::TranslationCache() : ctx_(std::make_unique<ExecContext>()) {
    ctx_->mem_read_slow = &jit_mem_read_slow;
    ctx_->mem_write_slow = &jit_mem_write_slow;
    ctx_->exec_complex = &jit_exec_complex;
    ctx_->flush_jump_table();
}

void TranslationCache::ensure_trampolines() {
    if (jit_entry_ != nullptr) {
        return;
    }
    const JitTrampolines tr = build_jit_trampolines();
    jit_entry_ = arena_.allocate(tr.entry_code.data(), tr.entry_code.size());
    jit_dispatch_ = arena_.allocate(tr.dispatch_code.data(), tr.dispatch_code.size());
    ctx_->jit_budget_epilogue = arena_.allocate(tr.budget_code.data(), tr.budget_code.size());
    ctx_->jit_exit_epilogue = arena_.allocate(tr.exit_code.data(), tr.exit_code.size());
}

const CachedBlock* TranslationCache::lookup(std::uint64_t pc) {
    const std::size_t slot = static_cast<std::size_t>((pc ^ (pc >> 12)) & kDirectMask);
    const std::size_t jslot = static_cast<std::size_t>((pc ^ (pc >> 12)) & kDirectJumpMask);
    const CachedBlock* fast = direct_[slot];
    if (fast != nullptr && fast->start == pc) {
        ++dispatch_hits_;
        ctx_->jump_table[jslot] = DirectJumpEntry{pc, fast->entry};
        return fast;
    }
    ++dispatch_misses_;
    const auto it = blocks_.find(pc);
    if (it == blocks_.end()) {
        return nullptr;
    }
    const CachedBlock* found = it->second.get();
    direct_[slot] = found;
    ctx_->jump_table[jslot] = DirectJumpEntry{pc, found->entry};
    return found;
}

void TranslationCache::install(const CachedBlock* block) {
    const std::size_t slot =
        static_cast<std::size_t>((block->start ^ (block->start >> 12)) & kDirectMask);
    const std::size_t jslot =
        static_cast<std::size_t>((block->start ^ (block->start >> 12)) & kDirectJumpMask);
    direct_[slot] = block;
    ctx_->jump_table[jslot] = DirectJumpEntry{block->start, block->entry};
}

void TranslationCache::invalidate_all() {
    blocks_.clear();
    std::fill(direct_.begin(), direct_.end(), nullptr);
    arena_.reset();
    jit_entry_ = nullptr;
    jit_dispatch_ = nullptr;
    if (ctx_ != nullptr) {
        ctx_->jit_exit_epilogue = nullptr;
        ctx_->flush_jump_table();
    }
}

const CachedBlock* TranslationCache::translate_at(Memory& memory, std::uint64_t pc,
                                                  TranslateFailure& failure) {
    failure = TranslateFailure::kNone;
    const CachedBlock* cached = lookup(pc);
    if (cached != nullptr) {
        return cached;
    }

    // Everything below is the miss path: decode, translate, allocate executable
    // memory, and install. Timed as a whole, because that whole is what a guest
    // pays the first time it reaches an address.
    const TranslateTimer translate_timer{translation_ns_};

    ensure_trampolines();

    if (fetch_.size() < kMaxWindow) {
        fetch_.resize(kMaxWindow);
    }
    const std::size_t available = read_window(memory, pc, fetch_.data(), fetch_.size());
    if (available == 0) {
        failure = TranslateFailure::kFault;
        return nullptr;
    }

    const DecodeResult decoded = decode(fetch_.data(), available, pc);
    if (!decoded.ok()) {
        failure = TranslateFailure::kUnsupported;
        return nullptr;
    }

    const BlockTranslation translated =
        translate_cached_block(fetch_.data(), available, decoded.instruction);
    if (!translated.ok() || translated.code.empty()) {
        failure = TranslateFailure::kUnsupported;
        return nullptr;
    }

    auto block = std::make_unique<CachedBlock>();
    block->entry = arena_.allocate(translated.code.data(), translated.code.size());
    block->exit = translated.exit;
    block->start = pc;
    block->target = translated.target;
    block->guest_instruction_count = translated.guest_instruction_count;
    block->code_bytes = translated.code.size();

    const CachedBlock* raw = block.get();
    blocks_.emplace(pc, std::move(block));
    install(raw);
    translated_guest_instructions_ += translated.guest_instruction_count;
    return raw;
}

std::size_t TranslationCache::total_code_bytes() const {
    std::size_t total = 0;
    for (const auto& entry : blocks_) {
        total += entry.second->code_bytes;
    }
    return total;
}

RunResult TranslationCache::run(Memory& memory, GuestState state, std::uint64_t max_steps,
                                SyscallHandler* syscalls) {
    RunResult out;
    out.state = state;

    if (!ExecMem::executable_supported()) {
        out.status = RunStatus::kUnsupported;
        out.message = "host cannot execute generated AArch64 code";
        return out;
    }

    const std::uint64_t entry_epoch = memory.code_epoch();
    if (cached_epoch_ != 0 && entry_epoch != cached_epoch_) {
        invalidate_all();
    }
    cached_epoch_ = entry_epoch;
    ctx_->memory = &memory;
    // The data SoftTlb is validated against the mapping generation, which changes
    // on *any* operation that moves or re-permissions a page; the translation
    // cache itself is invalidated by the code epoch, which changes when
    // executable memory is written. Conflating the two either flushed on every
    // syscall (~600 ns each) or, worse, left the data TLB holding freed pages.
    cached_map_epoch_ = memory.mapping_epoch();
    ctx_->tlb.flush(cached_map_epoch_);

    std::uint64_t steps = 0;
    while (true) {
        const std::uint64_t pc = out.state.rip;
        TranslateFailure failure = TranslateFailure::kNone;
        const CachedBlock* block = translate_at(memory, pc, failure);
        if (block == nullptr) {
            if (failure == TranslateFailure::kFault) {
                out.status = RunStatus::kFault;
                out.message = "instruction fetch fault at rip";
            } else {
                char hex_buf[32]{};
                std::snprintf(hex_buf, sizeof(hex_buf), "0x%llx",
                              static_cast<unsigned long long>(pc));
                out.status = RunStatus::kUnsupported;
                out.message = std::string("unsupported instruction at pc=") + hex_buf;
            }
            out.steps = steps;
            return out;
        }

        const std::uint64_t remaining = max_steps - steps;
        if (block->guest_instruction_count > remaining) {
            out.status = RunStatus::kStepLimit;
            out.message = "step limit reached without halting";
            out.steps = steps;
            return out;
        }

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto enter_fn = reinterpret_cast<EnterJitFn>(jit_entry_);
        const RawBlockReturn ret =
            enter_fn(&out.state, remaining, ctx_.get(), block->entry, jit_dispatch_);
        const auto status = static_cast<BlockResult>(ret.status);
        const std::uint64_t retired = ret.runs;
        steps += retired;
        if (retired > block->guest_instruction_count) {
            const std::uint64_t extra_blocks =
                (retired - block->guest_instruction_count) / block->guest_instruction_count;
            dispatch_hits_ += (extra_blocks > 0) ? extra_blocks : 1ULL;
        }

        if (status == BlockResult::kHalted) {
            out.status = RunStatus::kOk;
            out.steps = steps;
            return out;
        }
        if (status == BlockResult::kFault) {
            out.status = RunStatus::kFault;
            out.message = "guest memory or arithmetic fault";
            out.steps = steps;
            return out;
        }
        if (status == BlockResult::kSyscall) {
            if (syscalls == nullptr) {
                out.status = RunStatus::kUnsupported;
                out.message = "syscall encountered with no handler";
                out.steps = steps;
                return out;
            }
            const SyscallResult sr = syscalls->handle(out.state, memory);
            const std::uint64_t post_map_epoch = memory.mapping_epoch();
            if (post_map_epoch != cached_map_epoch_) {
                // The address space changed shape: every cached host page pointer
                // is suspect. Flush the data TLB, and - if executable memory moved
                // - drop the translations too.
                ctx_->tlb.flush(post_map_epoch);
                cached_map_epoch_ = post_map_epoch;
            }
            const std::uint64_t post_epoch = memory.code_epoch();
            if (post_epoch != cached_epoch_) {
                invalidate_all();
                cached_epoch_ = post_epoch;
            }
            if (sr.status == SyscallStatus::kExit) {
                out.status = RunStatus::kOk;
                out.exited_via_syscall = true;
                out.exit_code = sr.exit_code;
                out.message = "guest exited via syscall";
                out.steps = steps;
                return out;
            }
            if (sr.status == SyscallStatus::kUnimplemented) {
                ++out.unimplemented_syscalls;
            }
        }
    }
}

} // namespace hyperion
