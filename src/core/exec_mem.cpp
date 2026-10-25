// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "hyperion/exec_mem.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

namespace hyperion {

namespace {

std::size_t page_size() {
    const long p = ::sysconf(_SC_PAGESIZE);
    return p > 0 ? static_cast<std::size_t>(p) : 4096;
}

std::size_t round_up(std::size_t value, std::size_t multiple) {
    if (value > static_cast<std::size_t>(-1) - (multiple - 1)) {
        throw std::out_of_range("ExecMem: requested size overflows address calculation");
    }
    return ((value + multiple - 1) / multiple) * multiple;
}

} // namespace

ExecMem::ExecMem(std::size_t bytes) {
    size_ = round_up(bytes == 0 ? 1 : bytes, page_size());
    // PROT_READ|PROT_WRITE now; PROT_EXEC is added in finalize(). Asking for
    // RWX up front would defeat W^X and is unnecessary.
    void* p = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        throw std::runtime_error("ExecMem: mmap failed");
    }
    base_ = p;
}

ExecMem::~ExecMem() {
    release();
}

ExecMem::ExecMem(ExecMem&& other) noexcept
    : base_(other.base_), size_(other.size_), finalized_(other.finalized_) {
    other.base_ = nullptr;
    other.size_ = 0;
    other.finalized_ = false;
}

ExecMem& ExecMem::operator=(ExecMem&& other) noexcept {
    if (this != &other) {
        release();
        base_ = other.base_;
        size_ = other.size_;
        finalized_ = other.finalized_;
        other.base_ = nullptr;
        other.size_ = 0;
        other.finalized_ = false;
    }
    return *this;
}

void ExecMem::release() noexcept {
    if (base_ != nullptr) {
        ::munmap(base_, size_);
        base_ = nullptr;
        size_ = 0;
    }
}

std::uint8_t* ExecMem::writable() {
    if (finalized_) {
        throw std::logic_error("ExecMem: region is finalized (not writable)");
    }
    return static_cast<std::uint8_t*>(base_);
}

void ExecMem::write(std::size_t offset, const std::uint8_t* bytes, std::size_t count) {
    if (finalized_) {
        throw std::logic_error("ExecMem: cannot write after finalize()");
    }
    if (offset > size_ || count > size_ - offset) {
        throw std::out_of_range("ExecMem: write past end of region");
    }
    if (count > 0) {
        std::memcpy(writable() + offset, bytes, count);
    }
}

void ExecMem::finalize() {
    if (finalized_) {
        return;
    }
    if (::mprotect(base_, size_, PROT_READ | PROT_EXEC) != 0) {
        throw std::runtime_error("ExecMem: mprotect(RX) failed");
    }
    // Flush the instruction cache for the region. On AArch64 this is required
    // for correctness; on x86 it is a no-op but harmless.
    __builtin___clear_cache(static_cast<char*>(base_), static_cast<char*>(base_) + size_);
    finalized_ = true;
}

void* ExecMem::entry() const {
    if (!finalized_) {
        throw std::logic_error("ExecMem: entry() requires finalize() first");
    }
    return base_;
}

bool ExecMem::executable_supported() {
#if defined(__aarch64__)
    // Probe once whether this host process can actually obtain an RX mapping
    // under the active kernel/SELinux/seccomp policy (e.g. Android untrusted
    // app domains may deny PROT_EXEC on anonymous mappings).
    static const bool supported = [] {
        const std::size_t ps = page_size();
        void* p = ::mmap(nullptr, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            return false;
        }
        const bool ok = (::mprotect(p, ps, PROT_READ | PROT_EXEC) == 0);
        ::munmap(p, ps);
        return ok;
    }();
    return supported;
#else
    return false;
#endif
}

ExecArena::ExecArena(std::size_t slab_bytes)
    : slab_bytes_(round_up(slab_bytes == 0 ? 4096 : slab_bytes, page_size())) {}

ExecArena::~ExecArena() {
    for (std::size_t i = 0; i < slab_count_; ++i) {
        if (slabs_[i].base != nullptr) {
            ::munmap(slabs_[i].base, slabs_[i].capacity);
        }
    }
    delete[] slabs_;
}

void ExecArena::reset() noexcept {
    for (std::size_t i = 1; i < slab_count_; ++i) {
        if (slabs_[i].base != nullptr) {
            ::munmap(slabs_[i].base, slabs_[i].capacity);
            slabs_[i] = Slab{};
        }
    }
    if (slab_count_ > 0) {
        Slab& first = slabs_[0];
        const bool was_executable = first.published != 0;
        if (was_executable && ::mprotect(first.base, first.capacity, PROT_READ | PROT_WRITE) != 0) {
            // Slab 0 survives reset(), but publishing left it read-execute and it
            // has to be writable again before it can be reused. If that fails the
            // slab is unusable: drop it rather than hand out memory that will
            // fault on the next write. reset() is noexcept, so there is no throw
            // here; the next allocate() simply maps a fresh slab.
            ::munmap(first.base, first.capacity);
            slabs_[0] = Slab{};
            slab_count_ = 0;
            dirty_ = false;
            window_open_ = false;
            return;
        }
        if (was_executable) {
            ++protection_changes_;
        }
        first.used = 0;
        first.published = 0;
        slab_count_ = 1;
    }
    dirty_ = false;
    window_open_ = false;
}

void ExecArena::resize_slabs(std::size_t next_cap) {
    auto* grown = new Slab[next_cap];
    for (std::size_t i = 0; i < slab_count_; ++i) {
        grown[i] = slabs_[i];
    }
    delete[] slabs_;
    slabs_ = grown;
    slab_cap_ = next_cap;
}

void* ExecArena::allocate(const std::uint8_t* code, std::size_t size) {
    if (size == 0) {
        throw std::invalid_argument("ExecArena: zero-sized allocation");
    }
    const std::size_t aligned_size = round_up(size, 16);
    const std::size_t ps = page_size();

    // A published page is read-execute, so it can never be written again: move
    // the cursor past it. Anything left in the tail is wasted, which is why the
    // caller should have filled the window before publishing.
    Slab* target = nullptr;
    if (slab_count_ > 0) {
        Slab& cur = slabs_[slab_count_ - 1];
        if (cur.used < cur.published) {
            cur.used = cur.published;
        }
        if (aligned_size <= cur.capacity - cur.used) {
            target = &cur;
        }
    }

    if (target == nullptr) {
        const std::size_t cap =
            round_up(aligned_size > slab_bytes_ ? aligned_size : slab_bytes_, ps);
        // Created writable and non-executable. publish() adds PROT_EXEC; no page
        // is ever writable and executable at the same time.
        void* p = ::mmap(nullptr, cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            throw std::runtime_error("ExecArena: mmap failed");
        }
        if (slab_count_ == slab_cap_) {
            resize_slabs(slab_cap_ == 0 ? 4 : slab_cap_ * 2);
        }
        slabs_[slab_count_] = Slab{static_cast<std::uint8_t*>(p), cap, 0, 0};
        target = &slabs_[slab_count_++];
    }

    std::uint8_t* dst = target->base + target->used;
    std::memcpy(dst, code, size);
    target->used += aligned_size;
    dirty_ = true;

    if (!window_open_) {
        publish();
    }
    return dst;
}

void ExecArena::publish() {
    if (!dirty_) {
        window_open_ = false;
        return;
    }
    const std::size_t ps = page_size();
    for (std::size_t i = 0; i < slab_count_; ++i) {
        Slab& slab = slabs_[i];
        if (slab.used <= slab.published) {
            continue;
        }
        const std::size_t hi = round_up(slab.used, ps);
        const std::size_t span = hi - slab.published;
        std::uint8_t* page = slab.base + slab.published;
        if (::mprotect(page, span, PROT_READ | PROT_EXEC) != 0) {
            throw std::runtime_error("ExecArena: mprotect(RX) failed");
        }
        ++protection_changes_;
        // Only the bytes actually written need I-cache maintenance, but the
        // whole published span is cheap to cover and a missed range is a very
        // confusing stale-code bug on AArch64.
        __builtin___clear_cache(reinterpret_cast<char*>(page),
                                reinterpret_cast<char*>(page + span));
        slab.published = hi;
    }
    dirty_ = false;
    window_open_ = false;
}

} // namespace hyperion
