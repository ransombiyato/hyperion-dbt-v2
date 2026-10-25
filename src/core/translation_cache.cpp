// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Tier 1 dispatcher: manages `ExecArena`, shared AArch64 entry/dispatch/exit
// trampolines, `ExecContext` (`SoftTlb` + direct-mapped `jump_table`), and
// multi-block native execution.
#include "hyperion/translation_cache.hpp"

#include "hyperion/aarch64_encoder.hpp"

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

std::string hex_pc(std::uint64_t pc) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(pc));
    return std::string(buf);
}

bool jit_exec_complex(ExecContext* ctx, GuestState* state, const Instruction* insn) noexcept {
    ++ctx->helper_calls;
    // Force point 2 of 3 (see docs/translator-audit.md §29.11): the helper runs
    // the reference interpreter, which reads `state.flags` directly, so any
    // pending PF/AF descriptor has to be materialised before it can be observed.
    // The exit epilogue has already flushed on the way out of the block, so this
    // only fires for a descriptor recorded earlier in the *same* block.
    if (flush_pending_flags(*state)) {
        ++ctx->pf_af_flushes;
    }
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

bool TranslationCache::should_gate(std::uint64_t pc) {
    if (hot_threshold_ == 0) {
        return false;
    }
    const std::size_t slot = static_cast<std::size_t>((pc ^ (pc >> 12)) & kHotMask);
    HotEntry& entry = hot_[slot];
    if (entry.pc != pc) {
        entry.pc = pc;
        entry.count = 1;
        return true;
    }
    if (entry.count <= hot_threshold_) {
        const bool cold = entry.count < hot_threshold_;
        if (entry.count < 0xFFFF) {
            ++entry.count;
        }
        if (cold) {
            return true;
        }
        ++promoted_blocks_;
        return false;
    }
    return false;
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

    // P6: cold blocks are not worth compiling yet. Report it separately from a
    // translation failure so run() interprets rather than aborts.
    if (should_gate(pc)) {
        failure = TranslateFailure::kCold;
        return nullptr;
    }

    // One write window for the entry block and whatever successors can be
    // guessed, published once. Publishing per block costs two protection changes
    // per ~156-byte block, and those changes are 99.8% of the cost of publishing
    // a block (audit §29.6), so the window is what makes translation affordable.
    // publish() runs on every path, including the failure paths, so a block can
    // never be handed back non-executable.
    const TranslateTimer translate_timer{translation_ns_};
    arena_.begin_write();
    window_blocks_.clear();
    const CachedBlock* block = translate_one(memory, pc, failure);
    if (block != nullptr) {
        prefetch_successors(memory, *block);
    }
    // Both directions are needed before publish(): a successor's entry is only
    // known once it has been translated, and its exit can only be repointed while
    // its page is still writable. Doing it here is what makes direct linking
    // compatible with strict W^X - no published page is ever re-opened.
    for (CachedBlock* created : window_blocks_) {
        resolve_links(*created);
    }
    window_blocks_.clear();
    arena_.publish();
    return block;
}

const CachedBlock* TranslationCache::translate_one(Memory& memory, std::uint64_t pc,
                                                   TranslateFailure& failure) {
    failure = TranslateFailure::kNone;

    failure = TranslateFailure::kNone;
    const CachedBlock* cached = lookup(pc);
    if (cached != nullptr) {
        return cached;
    }

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
    block->exit_links = translated.exit_links;
    block->exit = translated.exit;
    block->start = pc;
    block->target = translated.target;
    block->guest_instruction_count = translated.guest_instruction_count;
    block->code_bytes = translated.code.size();
    block->pf_af_deferred = translated.pf_af_deferred;

    const CachedBlock* raw = block.get();
    window_blocks_.push_back(const_cast<CachedBlock*>(raw));
    blocks_.emplace(pc, std::move(block));
    install(raw);
    translated_guest_instructions_ += translated.guest_instruction_count;
    return raw;
}

std::uint64_t TranslationCache::helper_calls() const {
    return ctx_ != nullptr ? ctx_->helper_calls : 0;
}

std::uint64_t TranslationCache::pf_af_flushes() const {
    return ctx_ != nullptr ? ctx_->pf_af_flushes : 0;
}

std::uint64_t TranslationCache::pf_af_deferred() const {
    std::uint64_t total = 0;
    for (const auto& entry : blocks_) {
        total += entry.second->pf_af_deferred;
    }
    return total;
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
        if (block == nullptr && failure == TranslateFailure::kCold) {
            // P6: cold block. Execute exactly one instruction in the reference
            // interpreter, which is the same code tier 0 runs, and try again. The
            // step-limit accounting matches a block of one instruction.
            if (steps >= max_steps) {
                out.status = RunStatus::kStepLimit;
                out.message = "step limit reached without halting";
                out.steps = steps;
                return out;
            }
            const StepResult sr = step(out.state, memory, syscalls);
            ++steps;
            ++gated_steps_;
            out.unimplemented_syscalls += sr.unimplemented_syscalls;
            if (sr.instruction.opcode == Opcode::kSyscall) {
                // A syscall can move or re-permission memory. Same bookkeeping as
                // the block path does after a kSyscall result: the data TLB is
                // keyed on the mapping generation and the translations on the
                // code epoch.
                const std::uint64_t post_map_epoch = memory.mapping_epoch();
                if (post_map_epoch != cached_map_epoch_) {
                    ctx_->tlb.flush(post_map_epoch);
                    cached_map_epoch_ = post_map_epoch;
                }
                const std::uint64_t post_epoch = memory.code_epoch();
                if (post_epoch != cached_epoch_) {
                    invalidate_all();
                    cached_epoch_ = post_epoch;
                }
            }
            switch (sr.status) {
            case StepStatus::kOk:
                continue;
            case StepStatus::kHalted:
                out.status = RunStatus::kOk;
                out.steps = steps;
                out.exited_via_syscall = sr.exited_via_syscall;
                out.exit_code = sr.exit_code;
                return out;
            case StepStatus::kFault:
                out.status = RunStatus::kFault;
                out.message =
                    sr.message.empty() ? "guest fault while interpreting a cold block" : sr.message;
                out.steps = steps;
                return out;
            case StepStatus::kUnsupported:
                out.status = RunStatus::kUnsupported;
                out.message = std::string("unsupported instruction at pc=") + hex_pc(pc);
                out.steps = steps;
                return out;
            }
        }
        if (block == nullptr) {
            if (failure == TranslateFailure::kFault) {
                out.status = RunStatus::kFault;
                out.message = "instruction fetch fault at rip";
            } else {
                out.status = RunStatus::kUnsupported;
                out.message = std::string("unsupported instruction at pc=") + hex_pc(pc);
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

void TranslationCache::prefetch_successors(Memory& memory, const CachedBlock& block) {
    // Bounded, breadth-first guess at what runs next. The budget is measured in
    // generated bytes so a window roughly fills the page it touches (a partial
    // page left at publish time is wasted, because a published page can never be
    // written again), and capped in blocks so a wrong guess can never translate
    // an unbounded amount of dead code.
    constexpr std::size_t kMaxPrefetchBytes = 3584; // 7/8 of a 4 KiB page
    constexpr std::size_t kMaxPrefetchBlocks = 16;

    std::size_t budget = kMaxPrefetchBytes;
    std::size_t queued = 0;
    std::uint64_t pending[2 * kMaxPrefetchBlocks + 2];
    std::size_t pending_count = 0;

    const auto push = [&pending, &pending_count](std::uint64_t pc) {
        if (pending_count < sizeof(pending) / sizeof(pending[0])) {
            pending[pending_count++] = pc;
        }
    };
    const auto push_successors = [&push](const CachedBlock& b) {
        switch (b.exit) {
        case BlockExit::kHalted:
            break;
        case BlockExit::kJcc:
            push(b.target); // fall-through
            if (b.has_alternate) {
                push(b.alternate); // branch destination
            }
            break;
        case BlockExit::kSyscall:
        case BlockExit::kFallThrough:
        case BlockExit::kJmp:
            push(b.target);
            break;
        }
    };

    push_successors(block);
    while (pending_count != 0 && queued < kMaxPrefetchBlocks && budget != 0) {
        const std::uint64_t pc = pending[--pending_count];
        if (pc == 0 || lookup(pc) != nullptr) {
            continue; // already translated: nothing to buy, no cycle risk
        }
        TranslateFailure ignored = TranslateFailure::kNone;
        const CachedBlock* found = translate_one(memory, pc, ignored);
        if (found == nullptr) {
            continue; // a guess that did not pan out; the dispatcher will retry
        }
        ++queued;
        ++prefetched_blocks_;
        budget = found->code_bytes >= budget ? 0 : budget - found->code_bytes;
        push_successors(*found);
    }
}

void TranslationCache::resolve_links(CachedBlock& block) {
    if (block.exit_links.empty() || block.entry == nullptr) {
        return;
    }
    for (const ExitLink& link : block.exit_links) {
        const CachedBlock* successor = lookup(link.target_pc);
        if (successor == nullptr || successor->entry == nullptr) {
            continue; // not translated yet: the exit keeps its dispatcher branch
        }
        auto* site = static_cast<std::uint8_t*>(block.entry) + link.code_offset;
        const auto from = reinterpret_cast<std::intptr_t>(site);
        const auto to = reinterpret_cast<std::intptr_t>(successor->entry);
        const std::int64_t delta = static_cast<std::int64_t>(to - from);
        // 26-bit signed word offset: anything outside +-128 MiB stays on the
        // dispatcher rather than relying on a truncated branch displacement.
        if (delta < -(std::int64_t{1} << 27) || delta >= (std::int64_t{1} << 27)) {
            continue;
        }
        if (arena_.patch_instruction(site, a64::Encoder::encode_b(delta))) {
            ++linked_exits_;
        }
    }
}

} // namespace hyperion
