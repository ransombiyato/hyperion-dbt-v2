// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest virtual address space (Phase 6, increment 2). See address_space.hpp for
// the design rules and the security boundary this file implements.
#include "hyperion/address_space.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>

namespace hyperion {

namespace {
constexpr std::uint64_t kAlign = GuestAddressSpace::kPageSize;

// Allocate a region's backing bytes. A guest-controlled size can be large
// enough that the host allocation throws (std::bad_alloc / std::length_error);
// the caller turns that into a clean "no memory" outcome rather than letting an
// exception escape into the dispatcher and terminate the host process.
std::optional<std::vector<std::uint8_t>> try_allocate_bytes(std::size_t count) {
    try {
        return std::vector<std::uint8_t>(count, 0);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}
} // namespace

std::optional<std::uint64_t> GuestAddressSpace::page_align_up(std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - (kAlign - 1)) {
        return std::nullopt; // would overflow
    }
    return (value + (kAlign - 1)) & ~GuestAddressSpace::kPageMask;
}

void GuestAddressSpace::reset() {
    // Every mutating entry point bumps the mapping generation unconditionally.
    // This is the counter a SoftTlb validates against: it is about pages
    // moving, not about code changing, and getting it wrong for one branch is
    // exactly how the data TLB came to hold pointers into freed buffers. An
    // over-bump costs one 16 KiB flush; an under-bump costs wrong guest memory.
    ++mapping_epoch_;

    // Free the backing storage before re-arming the cursor: a program image can
    // be hundreds of MiB, and holding it while the new image is mapped would
    // double the transient host footprint on a small device.
    regions_.clear();
    regions_.shrink_to_fit();
    brk_ = heap_base_;
    mru_index_ = 0;
    // The mmap cursor is deliberately NOT rewound to the original mmap base.
    // execve() replaces the image but the address space object persists, so
    // rewinding would hand the new image addresses the outgoing image just
    // vacated; anything still holding a stale pointer to those addresses (a
    // SoftTlb entry the flush missed, a dangling guest pointer) would then alias
    // live data instead of faulting. Moving forward keeps the old addresses
    // unmapped and therefore faulting, which is the safe failure.
    ++code_epoch_;
}

GuestAddressSpace::GuestAddressSpace(std::uint64_t mmap_base, std::uint64_t heap_base,
                                     std::uint64_t heap_capacity, std::size_t max_map_bytes)
    : max_map_bytes_(max_map_bytes == 0 ? kDefaultMaxMapBytes : max_map_bytes) {
    mmap_cursor_ = mmap_base;
    heap_base_ = heap_base;
    heap_end_ = (heap_capacity <= std::numeric_limits<std::uint64_t>::max() - heap_base)
                    ? (heap_base + heap_capacity)
                    : std::numeric_limits<std::uint64_t>::max();
    brk_ = heap_base;
}

long GuestAddressSpace::find_region(std::uint64_t address) const {
    // Fast path: guest stack/code/data accesses demonstrate strong spatial and
    // temporal locality, so check the most recently matched region first.
    if (mru_index_ < regions_.size()) {
        const Region& cached = regions_[mru_index_];
        if (address >= cached.base && address < cached.end()) {
            return static_cast<long>(mru_index_);
        }
    }
    // Regions are sorted by base and non-overlapping; binary search for the last
    // region whose base is <= address.
    const auto it =
        std::upper_bound(regions_.begin(), regions_.end(), address,
                         [](std::uint64_t addr, const Region& r) { return addr < r.base; });
    if (it == regions_.begin()) {
        return -1;
    }
    const long index = static_cast<long>(std::distance(regions_.begin(), it)) - 1;
    const Region& r = regions_[static_cast<std::size_t>(index)];
    if (address < r.end()) {
        mru_index_ = static_cast<std::size_t>(index);
        return index;
    }
    return -1;
}

bool GuestAddressSpace::split_region_at(std::uint64_t boundary) {
    const long idx = find_region(boundary);
    if (idx < 0) {
        return true;
    }
    Region& r = regions_[static_cast<std::size_t>(idx)];
    if (boundary <= r.base || boundary >= r.end()) {
        return true;
    }
    const auto left_len = static_cast<std::size_t>(boundary - r.base);
    const auto right_len = static_cast<std::size_t>(r.end() - boundary);
    auto right_bytes = try_allocate_bytes(right_len);
    if (!right_bytes) {
        return false;
    }
    std::memcpy(right_bytes->data(), r.bytes.data() + left_len, right_len);

    Region tail;
    tail.base = boundary;
    tail.size = right_len;
    tail.perm = r.perm;
    tail.bytes = std::move(*right_bytes);

    r.bytes.resize(left_len);
    r.size = left_len;

    regions_.insert(regions_.begin() + (idx + 1), std::move(tail));
    return true;
}

bool GuestAddressSpace::range_is_free(std::uint64_t addr, std::uint64_t size) const {
    if (addr > std::numeric_limits<std::uint64_t>::max() - size) {
        return false;
    }
    const std::uint64_t end = addr + size;
    if (heap_base_ < heap_end_ && addr < heap_end_ && heap_base_ < end) {
        return false; // reserved for brk heap growth
    }
    for (const Region& r : regions_) {
        if (addr < r.end() && r.base < end) {
            return false; // overlap
        }
    }
    return true;
}

bool GuestAddressSpace::map_image(std::uint64_t base, const void* data, std::size_t size,
                                  MemPerm perm) {
    // Every mutating entry point bumps the mapping generation unconditionally.
    // This is the counter a SoftTlb validates against: it is about pages
    // moving, not about code changing, and getting it wrong for one branch is
    // exactly how the data TLB came to hold pointers into freed buffers. An
    // over-bump costs one 16 KiB flush; an under-bump costs wrong guest memory.
    ++mapping_epoch_;

    if ((base & kPageMask) != 0) {
        return false;
    }
    if (size > max_map_bytes_) {
        return false;
    }
    const auto mapped_size = page_align_up(size == 0 ? 1 : size);
    if (!mapped_size || *mapped_size > max_map_bytes_) {
        return false;
    }
    if (base > std::numeric_limits<std::uint64_t>::max() - *mapped_size) {
        return false;
    }
    if (!range_is_free(base, *mapped_size)) {
        return false;
    }

    Region region;
    region.base = base;
    region.size = *mapped_size;
    region.perm = perm;
    auto bytes = try_allocate_bytes(static_cast<std::size_t>(*mapped_size));
    if (!bytes) {
        return false;
    }
    region.bytes = std::move(*bytes);
    if (data != nullptr && size > 0) {
        std::memcpy(region.bytes.data(), data, size);
    }

    regions_.push_back(std::move(region));
    std::sort(regions_.begin(), regions_.end(),
              [](const Region& a, const Region& b) { return a.base < b.base; });
    if (has_perm(perm, MemPerm::kExec)) {
        ++code_epoch_;
    }
    return true;
}

bool GuestAddressSpace::map_segment(std::uint64_t vaddr, const void* data, std::size_t filesz,
                                    std::size_t memsz, MemPerm perm) {
    // Every mutating entry point bumps the mapping generation unconditionally.
    // This is the counter a SoftTlb validates against: it is about pages
    // moving, not about code changing, and getting it wrong for one branch is
    // exactly how the data TLB came to hold pointers into freed buffers. An
    // over-bump costs one 16 KiB flush; an under-bump costs wrong guest memory.
    ++mapping_epoch_;

    const auto* bytes_in = static_cast<const std::uint8_t*>(data);
    if (filesz > memsz || memsz == 0) {
        return false;
    }
    // A segment larger than the per-mapping bound is refused for the same reason
    // an oversized mmap is (see SECURITY.md): the backing store is host memory
    // and the size is attacker-controlled.
    if (memsz > max_map_bytes_) {
        return false;
    }
    // Map the whole pages the segment touches: [page_start, end).
    const std::uint64_t page_start = vaddr & ~kPageMask;
    const std::uint64_t in_page = vaddr - page_start; // segment offset in its first page
    if (memsz > std::numeric_limits<std::uint64_t>::max() - in_page) {
        return false;
    }
    const auto span = page_align_up(in_page + memsz);
    if (!span) {
        return false;
    }
    if (page_start > std::numeric_limits<std::uint64_t>::max() - *span) {
        return false;
    }
    const std::uint64_t end = page_start + *span;
    const std::uint64_t file_end = vaddr + filesz; // <= vaddr + memsz, no overflow (checked above)

    // Copy the file bytes for [from, to) that belong to this segment, into the
    // region that owns them. `from`/`to` are guest addresses.
    const auto copy_file_bytes = [&](Region& region, std::uint64_t from, std::uint64_t to) {
        if (bytes_in == nullptr) {
            return;
        }
        from = std::max(from, vaddr);
        to = std::min(to, file_end);
        if (from >= to) {
            return;
        }
        std::memcpy(region.bytes.data() + (from - region.base), bytes_in + (from - vaddr),
                    static_cast<std::size_t>(to - from));
    };

    // First, walk any already-mapped pages the segment overlaps and merge into
    // them (a linker that does not use `-z separate-code` can put the tail of one
    // PT_LOAD and the head of the next in the same page). The shared page keeps
    // its bytes and its permissions become the union of both segments'.
    std::uint64_t cursor = page_start;
    while (cursor < end) {
        const long idx = find_region(cursor);
        if (idx < 0) {
            break; // first unmapped page: map the rest in one go below
        }
        Region& region = regions_[static_cast<std::size_t>(idx)];
        const std::uint64_t overlap_end = std::min(end, region.end());
        copy_file_bytes(region, cursor, overlap_end);
        region.perm = region.perm | perm; // union: never take access away
        cursor = overlap_end;
    }

    if (cursor >= end) {
        return true; // the whole range was shared with existing mappings
    }

    // Map the remainder [cursor, end) as a fresh region, zero-filled (BSS) except
    // for the file bytes copied in.
    const auto remaining = end - cursor;
    Region region;
    region.base = cursor;
    region.size = remaining;
    region.perm = perm;
    auto bytes = try_allocate_bytes(static_cast<std::size_t>(remaining));
    if (!bytes) {
        return false;
    }
    region.bytes = std::move(*bytes);
    copy_file_bytes(region, cursor, end);

    regions_.push_back(std::move(region));
    std::sort(regions_.begin(), regions_.end(),
              [](const Region& a, const Region& b) { return a.base < b.base; });
    return true;
}

std::optional<std::uint64_t> GuestAddressSpace::map_anonymous(std::size_t size, MemPerm perm,
                                                              std::uint64_t hint) {
    // Every mutating entry point bumps the mapping generation unconditionally.
    // This is the counter a SoftTlb validates against: it is about pages
    // moving, not about code changing, and getting it wrong for one branch is
    // exactly how the data TLB came to hold pointers into freed buffers. An
    // over-bump costs one 16 KiB flush; an under-bump costs wrong guest memory.
    ++mapping_epoch_;

    if (size == 0) {
        return std::nullopt;
    }
    // Refuse an oversized request up front. Without this, the backing
    // allocation below would be sized by a guest-controlled value: a huge mmap
    // would either throw and terminate the host process or commit gigabytes of
    // host memory (see SECURITY.md).
    if (size > max_map_bytes_) {
        return std::nullopt;
    }
    const auto mapped_size = page_align_up(size);
    if (!mapped_size || *mapped_size > max_map_bytes_) {
        return std::nullopt;
    }

    std::uint64_t base = 0;
    if (hint != 0 && (hint & kPageMask) == 0 &&
        hint <= std::numeric_limits<std::uint64_t>::max() - *mapped_size &&
        range_is_free(hint, *mapped_size)) {
        base = hint;
    } else {
        // First fit at or above the cursor.
        std::uint64_t candidate = mmap_cursor_;
        while (candidate <= std::numeric_limits<std::uint64_t>::max() - *mapped_size) {
            if (range_is_free(candidate, *mapped_size)) {
                base = candidate;
                break;
            }
            if (heap_base_ < heap_end_ && candidate < heap_end_ &&
                heap_base_ < candidate + *mapped_size) {
                candidate = heap_end_;
                continue;
            }
            const long idx = find_region(candidate);
            if (idx >= 0) {
                candidate = regions_[static_cast<std::size_t>(idx)].end();
            } else {
                // Jump directly past whichever region overlaps [candidate, candidate +
                // *mapped_size).
                bool advanced = false;
                for (const Region& r : regions_) {
                    if (candidate < r.end() && r.base < candidate + *mapped_size) {
                        candidate = r.end();
                        advanced = true;
                        break;
                    }
                }
                if (!advanced) {
                    break;
                }
            }
        }
        if (base == 0) {
            return std::nullopt; // no space found
        }
    }

    Region region;
    region.base = base;
    region.size = *mapped_size;
    region.perm = perm;
    auto bytes = try_allocate_bytes(static_cast<std::size_t>(*mapped_size));
    if (!bytes) {
        return std::nullopt; // allocation failed: report "no memory", do not throw
    }
    region.bytes = std::move(*bytes);
    regions_.push_back(std::move(region));
    std::sort(regions_.begin(), regions_.end(),
              [](const Region& a, const Region& b) { return a.base < b.base; });
    if (base + *mapped_size > mmap_cursor_) {
        mmap_cursor_ = base + *mapped_size;
    }
    if (has_perm(perm, MemPerm::kExec)) {
        ++code_epoch_;
    }
    return base;
}

bool GuestAddressSpace::map_fixed(std::uint64_t addr, std::size_t size, MemPerm perm,
                                  const void* init_data, std::size_t init_len) {
    // Every mutating entry point bumps the mapping generation unconditionally.
    // This is the counter a SoftTlb validates against: it is about pages
    // moving, not about code changing, and getting it wrong for one branch is
    // exactly how the data TLB came to hold pointers into freed buffers. An
    // over-bump costs one 16 KiB flush; an under-bump costs wrong guest memory.
    ++mapping_epoch_;

    if (size == 0 || (addr & kPageMask) != 0 || size > max_map_bytes_ || init_len > size) {
        return false;
    }
    const auto mapped_size = page_align_up(size);
    if (!mapped_size || *mapped_size > max_map_bytes_ ||
        addr > std::numeric_limits<std::uint64_t>::max() - *mapped_size) {
        return false;
    }
    const std::uint64_t end = addr + *mapped_size;
    if (heap_base_ < heap_end_ && addr < heap_end_ && heap_base_ < end) {
        return false;
    }
    if (!split_region_at(addr) || !split_region_at(end)) {
        return false;
    }
    bool exec_changed = has_perm(perm, MemPerm::kExec);
    for (const Region& r : regions_) {
        if (r.base >= addr && r.end() <= end && has_perm(r.perm, MemPerm::kExec)) {
            exec_changed = true;
        }
    }
    auto bytes = try_allocate_bytes(static_cast<std::size_t>(*mapped_size));
    if (!bytes) {
        return false;
    }
    if (init_data != nullptr && init_len > 0) {
        std::memcpy(bytes->data(), init_data, init_len);
    }
    regions_.erase(
        std::remove_if(regions_.begin(), regions_.end(),
                       [&](const Region& r) { return r.base >= addr && r.end() <= end; }),
        regions_.end());

    Region region;
    region.base = addr;
    region.size = *mapped_size;
    region.perm = perm;
    region.bytes = std::move(*bytes);
    regions_.push_back(std::move(region));
    std::sort(regions_.begin(), regions_.end(),
              [](const Region& a, const Region& b) { return a.base < b.base; });
    if (exec_changed) {
        ++code_epoch_;
    }
    return true;
}

bool GuestAddressSpace::unmap(std::uint64_t addr, std::size_t size) {
    // Every mutating entry point bumps the mapping generation unconditionally.
    // This is the counter a SoftTlb validates against: it is about pages
    // moving, not about code changing, and getting it wrong for one branch is
    // exactly how the data TLB came to hold pointers into freed buffers. An
    // over-bump costs one 16 KiB flush; an under-bump costs wrong guest memory.
    ++mapping_epoch_;

    if (size == 0 || (addr & kPageMask) != 0) {
        return false;
    }
    const auto mapped_size = page_align_up(size);
    if (!mapped_size || addr > std::numeric_limits<std::uint64_t>::max() - *mapped_size) {
        return false;
    }
    const std::uint64_t end = addr + *mapped_size;

    // Verify every page in [addr, end) is currently mapped.
    std::uint64_t cursor = addr;
    bool had_exec = false;
    while (cursor < end) {
        const long idx = find_region(cursor);
        if (idx < 0) {
            return false;
        }
        const Region& r = regions_[static_cast<std::size_t>(idx)];
        had_exec = had_exec || has_perm(r.perm, MemPerm::kExec);
        cursor = std::min(r.end(), end);
    }

    // Split any regions spanning `addr` or `end` so removing [addr, end)
    // preserves surrounding pages and their contents intact.
    if (!split_region_at(addr) || !split_region_at(end)) {
        return false;
    }

    regions_.erase(
        std::remove_if(regions_.begin(), regions_.end(),
                       [&](const Region& r) { return r.base >= addr && r.end() <= end; }),
        regions_.end());
    if (had_exec) {
        ++code_epoch_;
    }
    return true;
}

bool GuestAddressSpace::protect(std::uint64_t addr, std::size_t size, MemPerm perm) {
    // Every mutating entry point bumps the mapping generation unconditionally.
    // This is the counter a SoftTlb validates against: it is about pages
    // moving, not about code changing, and getting it wrong for one branch is
    // exactly how the data TLB came to hold pointers into freed buffers. An
    // over-bump costs one 16 KiB flush; an under-bump costs wrong guest memory.
    ++mapping_epoch_;

    if (size == 0 || (addr & kPageMask) != 0) {
        return false;
    }
    const auto mapped_size = page_align_up(size);
    if (!mapped_size || addr > std::numeric_limits<std::uint64_t>::max() - *mapped_size) {
        return false;
    }
    const std::uint64_t end = addr + *mapped_size;

    // Every page in the range must be mapped; a hole makes the call fail.
    std::uint64_t cursor = addr;
    while (cursor < end) {
        const long idx = find_region(cursor);
        if (idx < 0) {
            return false;
        }
        cursor = std::min(regions_[static_cast<std::size_t>(idx)].end(), end);
    }

    // Split at range boundaries so sub-range mprotect never mutates permissions
    // of adjacent pages in the same mapping.
    if (!split_region_at(addr) || !split_region_at(end)) {
        return false;
    }

    bool exec_changed = has_perm(perm, MemPerm::kExec);
    for (Region& r : regions_) {
        if (r.base >= addr && r.end() <= end) {
            exec_changed = exec_changed || has_perm(r.perm, MemPerm::kExec);
            r.perm = perm;
        }
    }
    if (exec_changed) {
        ++code_epoch_;
    }
    return true;
}

std::uint64_t GuestAddressSpace::set_brk(std::uint64_t requested) {
    // Every mutating entry point bumps the mapping generation unconditionally.
    // This is the counter a SoftTlb validates against: it is about pages
    // moving, not about code changing, and getting it wrong for one branch is
    // exactly how the data TLB came to hold pointers into freed buffers. An
    // over-bump costs one 16 KiB flush; an under-bump costs wrong guest memory.
    ++mapping_epoch_;

    if (requested < heap_base_ || requested > heap_end_) {
        return brk_; // unchanged: the request is outside the heap area
    }
    if (requested > brk_) {
        if (!ensure_heap_mapped(requested)) {
            return brk_; // Linux contract: on ENOMEM return unchanged break
        }
        brk_ = requested;
        return brk_;
    }
    if (requested < brk_) {
        const long idx = find_region(heap_base_);
        if (idx >= 0) {
            Region& heap = regions_[static_cast<std::size_t>(idx)];
            const auto new_aligned = page_align_up(requested);
            if (requested == heap_base_) {
                regions_.erase(regions_.begin() + idx);
            } else if (new_aligned && *new_aligned >= heap.base) {
                const auto kept_pages = static_cast<std::size_t>(*new_aligned - heap.base);
                const auto zero_from = static_cast<std::size_t>(requested - heap.base);
                if (kept_pages < heap.bytes.size()) {
                    heap.bytes.resize(kept_pages);
                    heap.size = kept_pages;
                }
                if (zero_from < heap.bytes.size()) {
                    std::memset(heap.bytes.data() + zero_from, 0, heap.bytes.size() - zero_from);
                }
            }
        }
        brk_ = requested;
    }
    return brk_;
}

bool GuestAddressSpace::ensure_heap_mapped(std::uint64_t upto) {
    // Every mutating entry point bumps the mapping generation unconditionally.
    // This is the counter a SoftTlb validates against: it is about pages
    // moving, not about code changing, and getting it wrong for one branch is
    // exactly how the data TLB came to hold pointers into freed buffers. An
    // over-bump costs one 16 KiB flush; an under-bump costs wrong guest memory.
    ++mapping_epoch_;

    if (upto <= heap_base_) {
        return true;
    }
    const auto aligned = page_align_up(upto);
    if (!aligned) {
        return false;
    }
    const std::uint64_t needed = *aligned - heap_base_;
    if (needed > max_map_bytes_) {
        return false;
    }

    const long idx = find_region(heap_base_);
    if (idx >= 0) {
        Region& heap = regions_[static_cast<std::size_t>(idx)];
        if (heap.base != heap_base_) {
            return false;
        }
        if (heap.size < needed) {
            try {
                // Resize preserves all existing heap bytes [0, old_size) and
                // zero-fills newly mapped pages [old_size, needed).
                heap.bytes.resize(static_cast<std::size_t>(needed), 0);
            } catch (const std::exception&) {
                return false;
            }
            heap.size = needed;
        }
        return true;
    }

    auto bytes = try_allocate_bytes(static_cast<std::size_t>(needed));
    if (!bytes) {
        return false;
    }
    Region heap;
    heap.base = heap_base_;
    heap.size = needed;
    heap.perm = MemPerm::kRead | MemPerm::kWrite;
    heap.bytes = std::move(*bytes);
    regions_.push_back(std::move(heap));
    std::sort(regions_.begin(), regions_.end(),
              [](const Region& a, const Region& b) { return a.base < b.base; });
    return true;
}

std::optional<MemPerm> GuestAddressSpace::perm_at(std::uint64_t address) const {
    const long idx = find_region(address);
    if (idx < 0) {
        return std::nullopt;
    }
    return regions_[static_cast<std::size_t>(idx)].perm;
}

bool GuestAddressSpace::access(std::uint64_t address, std::uint8_t* buf, std::size_t count,
                               MemPerm need, bool is_write) {
    if (count == 0) {
        return true;
    }
    if (address > std::numeric_limits<std::uint64_t>::max() - count) {
        return false; // wrap-around
    }
    const std::uint64_t end = address + count;

    std::uint64_t cursor = address;
    while (cursor < end) {
        const long idx = find_region(cursor);
        if (idx < 0) {
            return false; // unmapped
        }
        Region& r = regions_[static_cast<std::size_t>(idx)];
        if (!has_perm(r.perm, need)) {
            return false; // mapped but not accessible this way
        }
        const std::uint64_t stop = std::min(r.end(), end);
        const std::size_t offset = static_cast<std::size_t>(cursor - r.base);
        const std::size_t n = static_cast<std::size_t>(stop - cursor);
        if (is_write) {
            std::memcpy(r.bytes.data() + offset, buf + (cursor - address), n);
            if (has_perm(r.perm, MemPerm::kExec)) {
                ++code_epoch_;
            }
        } else {
            std::memcpy(buf + (cursor - address), r.bytes.data() + offset, n);
        }
        cursor = stop;
    }
    return true;
}

PageAccess GuestAddressSpace::translate_page(std::uint64_t page_addr) noexcept {
    if ((page_addr & kPageMask) != 0) {
        return {};
    }
    const long idx = find_region(page_addr);
    if (idx < 0) {
        return {};
    }
    Region& r = regions_[static_cast<std::size_t>(idx)];
    if (page_addr < r.base || r.end() - page_addr < kPageSize) {
        return {};
    }
    PageAccess out;
    out.host_page = r.bytes.data() + static_cast<std::size_t>(page_addr - r.base);
    out.can_read = has_perm(r.perm, MemPerm::kRead);
    out.can_write_fast = has_perm(r.perm, MemPerm::kWrite) && !has_perm(r.perm, MemPerm::kExec);
    return out;
}

bool GuestAddressSpace::read(std::uint64_t address, std::uint8_t* out, std::size_t count) {
    return access(address, out, count, MemPerm::kRead, /*is_write=*/false);
}

bool GuestAddressSpace::write(std::uint64_t address, const std::uint8_t* in, std::size_t count) {
    return access(address, const_cast<std::uint8_t*>(in), count, MemPerm::kWrite,
                  /*is_write=*/true);
}

bool GuestAddressSpace::fetch(std::uint64_t address, std::uint8_t* out, std::size_t count) {
    return access(address, out, count, MemPerm::kExec, /*is_write=*/false);
}

} // namespace hyperion
