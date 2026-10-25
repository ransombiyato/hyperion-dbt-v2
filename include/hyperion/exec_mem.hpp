// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Executable memory for the Phase 3 code generator.
//
// Design (see DECISIONS.md D-016 and SECURITY.md):
//   - Memory is allocated read-write, filled, then made read-execute. It is
//     never simultaneously writable and executable, so the translator does not
//     need to disable W^X to work.
//   - After making the region executable the instruction cache is cleared.
//     On AArch64 a missing I-cache maintenance step produces stale code
//     execution, which is a classic and very confusing bug.
//   - The region is owned and freed on destruction; nothing is leaked and no
//     address is ever handed out that outlives the object.
#ifndef HYPERION_EXEC_MEM_HPP
#define HYPERION_EXEC_MEM_HPP

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace hyperion {

// A page-aligned region that can be written and then executed.
class ExecMem {
public:
    explicit ExecMem(std::size_t bytes);
    ~ExecMem();
    ExecMem(const ExecMem&) = delete;
    ExecMem& operator=(const ExecMem&) = delete;
    ExecMem(ExecMem&& other) noexcept;
    ExecMem& operator=(ExecMem&& other) noexcept;

    // Writable view. Valid only before finalize().
    std::uint8_t* writable();
    std::size_t size() const { return size_; }

    // Copy `bytes` into the region at `offset`. Throws if it would not fit or
    // if the region has already been finalized.
    void write(std::size_t offset, const std::uint8_t* bytes, std::size_t count);

    // Make the region read-execute and flush the instruction cache. Idempotent.
    void finalize();

    // Address of the code, suitable for calling. Requires finalize() first.
    void* entry() const;

    // Whether this platform can execute memory the program generated. False on
    // a host whose ISA differs from the target, and false if W^X is enforced
    // without a way to obtain an executable mapping.
    static bool executable_supported();

private:
    void release() noexcept;

    void* base_ = nullptr;
    std::size_t size_ = 0;
    bool finalized_ = false;
};

// Reusable W^X arena for translated basic blocks.
//
// Slabs are created read-write. Code written into a slab stays writable and
// non-executable until `publish()` flips the pages it occupies to read-execute:
// no page is ever both writable and executable, so this is W^X in the strict
// page-table sense, not W^X-by-alias.
//
// The write window exists because a published page cannot be written again. One
// `publish()` therefore has to cover everything that should become executable
// together: with a ~156-byte basic block, publishing per block costs two
// protection changes per block (audit §29.6: 2.787 us, 99.8% of the cost of
// publishing a block), while publishing a window's worth of blocks costs one.
//
// Contract: memory handed out while a window is open is NOT executable until
// `publish()` returns. Callers that execute generated code must publish first.
// `allocate()` without an open window does both, so the simple one-block use is
// unchanged and cannot be mistaken for the batched one.
class ExecArena {
public:
    explicit ExecArena(std::size_t slab_bytes = 64ULL * 1024ULL);
    ~ExecArena();
    ExecArena(const ExecArena&) = delete;
    ExecArena& operator=(const ExecArena&) = delete;

    // Copy `size` bytes into the arena and return their address. With no write
    // window open this also publishes, so the result is executable on return;
    // inside a window (see begin_write()) the memory stays writable and
    // non-executable until publish().
    void* allocate(const std::uint8_t* code, std::size_t size);

    // Open a write window: subsequent allocate() calls are writable but not
    // executable, and are made executable together by publish().
    void begin_write() noexcept { window_open_ = true; }
    bool write_window_open() const noexcept { return window_open_; }

    // Flip everything written since the last publish() to read-execute and flush
    // the instruction cache. Idempotent; a window with nothing new written costs
    // nothing. Any bytes left in a published page become unusable, so a window
    // should cover enough code to fill the pages it touches.
    void publish();

    // Number of mprotect calls the arena has made. Exposed so a benchmark can
    // report the cost the audit is about instead of asserting it (§29.6).
    std::uint64_t protection_changes() const noexcept { return protection_changes_; }

    // Drop all live allocations and reuse the first slab on subsequent runs.
    void reset() noexcept;

private:
    struct Slab {
        std::uint8_t* base = nullptr;
        std::size_t capacity = 0;
        std::size_t used = 0;
        // Bytes of this slab already flipped to RX. Always page-aligned; the
        // cursor never moves backwards, so published bytes are never rewritten.
        std::size_t published = 0;
    };

    void resize_slabs(std::size_t next_cap);

    std::size_t slab_bytes_ = 0;
    Slab* slabs_ = nullptr;
    std::size_t slab_count_ = 0;
    std::size_t slab_cap_ = 0;
    bool window_open_ = false;
    bool dirty_ = false;
    std::uint64_t protection_changes_ = 0;
};

} // namespace hyperion

#endif // HYPERION_EXEC_MEM_HPP
