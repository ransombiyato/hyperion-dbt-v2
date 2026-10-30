// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest virtual address space (Phase 6, increment 2).
//
// Until now the guest ran against a single flat FlatMemory region supplied by
// the caller. Real Linux programs need more than that: a program image, a
// growable heap (brk), and anonymous mappings with distinct permissions
// (mmap/mprotect). This class is the guest's *own* address space: a sparse,
// page-granular set of regions, each with its own permissions, that implements
// the same Memory interface the interpreter and translator already use.
//
// Design rules (see DECISIONS.md D-023 and SECURITY.md):
//   - The guest never receives a host pointer. Every access is validated
//     against the region table, so a bad guest pointer becomes a fault, never a
//     host read/write out of bounds.
//   - Permissions are enforced on every access. Writing to a read-only or
//     PROT_NONE region fails, exactly as the kernel would fault.
//   - All ranges are page-aligned and overflow-checked before any arithmetic.
//     A guest-supplied size cannot wrap around the address space.
//   - The address space is *described* by abstract guest addresses; the backing
//     bytes live in host memory owned by this object. A guest address is never
//     dereferenced as a host address.
#ifndef HYPERION_ADDRESS_SPACE_HPP
#define HYPERION_ADDRESS_SPACE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "hyperion/interpreter.hpp" // Memory

namespace hyperion {

// Page permissions, mirroring the PROT_* bits the guest passes to mprotect and
// mmap. kNone is PROT_NONE: mapped but inaccessible. All bitmask combinations
// 0..7 are named explicitly so static analysis knows the full value domain.
enum class MemPerm : std::uint8_t {
    kNone = 0,
    kRead = 1,
    kWrite = 2,
    kReadWrite = 3,
    kExec = 4,
    kReadExec = 5,
    kWriteExec = 6,
    kAll = 7,
};

constexpr MemPerm operator|(MemPerm a, MemPerm b) noexcept {
    return static_cast<MemPerm>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr bool has_perm(MemPerm set, MemPerm bit) noexcept {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(bit)) != 0;
}

// A page-aligned, permission-checked guest address space.
class GuestAddressSpace final : public Memory {
public:
    static constexpr std::uint64_t kPageSize = 4096;
    static constexpr std::uint64_t kPageMask = kPageSize - 1;

    // `mmap_base` is the first address the anonymous-mapping allocator will
    // consider; `heap_base`/`heap_capacity` reserve the brk area. These are
    // abstract guest addresses, chosen to be disjoint from typical image bases.
    //
    // `max_map_bytes` bounds a single mapping's size. It exists because the
    // backing store is host memory: a guest that asks for an enormous
    // anonymous mapping must be refused with ENOMEM rather than driving a
    // host-side allocation (see SECURITY.md). The default is generous for
    // ordinary programs but far below what would exhaust a small device.
    static constexpr std::size_t kDefaultMaxMapBytes = 1ULL << 30; // 1 GiB
    explicit GuestAddressSpace(std::uint64_t mmap_base = 0x7f0000000000ULL,
                               std::uint64_t heap_base = 0x200000ULL,
                               std::uint64_t heap_capacity = 1ULL << 20,
                               std::size_t max_map_bytes = kDefaultMaxMapBytes);

    // Deep copy. fork(2) uses this to give a guest child a private address
    // space: the guest's pages are backed by host heap storage owned here, so a
    // host fork alone would leave parent and child sharing the *same* vectors
    // until either one mmap/munmaps and reallocates them, invalidating the
    // other's cached host page pointers. Copying is what makes the two address
    // spaces genuinely independent. The copy reserves up front so the region
    // vectors are not re-grown page group by page group.
    GuestAddressSpace(const GuestAddressSpace& other)
        : regions_(other.regions_), mmap_cursor_(other.mmap_cursor_), heap_base_(other.heap_base_),
          heap_end_(other.heap_end_), brk_(other.brk_), max_map_bytes_(other.max_map_bytes_),
          code_epoch_(other.code_epoch_) {
        regions_.reserve(other.regions_.size());
    }
    GuestAddressSpace& operator=(const GuestAddressSpace&) = delete;
    GuestAddressSpace(GuestAddressSpace&&) = delete;
    GuestAddressSpace& operator=(GuestAddressSpace&&) = delete;

    // Discard every mapping and return the space to the state its constructor
    // produced, preserving the geometry (mmap base, heap area, per-mapping
    // bound). execve(2) uses this to drop the outgoing image: the executor holds
    // a reference to *this* object, so replacing it is not an option, and
    // clearing it in place is what lets a new image be loaded at the same
    // address. Bumps code_epoch() so both execution tiers invalidate their
    // translations of the image that no longer exists.
    void reset();

    // Map the program image at `base` (page-aligned) with the given
    // permissions and copy `size` bytes of `data` into it. Returns false if the
    // range is invalid, misaligned, or overlaps an existing mapping.
    bool map_image(std::uint64_t base, const void* data, std::size_t size, MemPerm perm);

    // Map a loaded segment: `memsz` bytes starting at guest address `vaddr`, of
    // which the first `filesz` are copied from `data` and the remainder is zero
    // (BSS). The mapping is page-granular, so `vaddr` need not be page-aligned
    // (a PT_LOAD's `p_vaddr` only has to satisfy `p_vaddr ≡ p_offset (mod
    // p_align)`, and the last segment of a real binary usually is not aligned);
    // the containing page is mapped and the bytes placed at their offset within
    // it.
    //
    // A segment may share a page with an already-mapped segment (a linker that
    // does not use `-z separate-code` can put the tail of one PT_LOAD and the
    // head of the next in one page). Sharing is handled by *merging*: the shared
    // page keeps its bytes and its permissions become the union of both
    // segments'. Union never under-permits; for a page that would be both
    // writable and executable it is a deliberate W^X relaxation, noted in
    // docs/known-issues.md. Binaries linked with `-z separate-code` (the modern
    // default) have no shared pages, so no relaxation occurs.
    //
    // Returns false if the range is invalid, overflows, exceeds the per-mapping
    // bound, or cannot be allocated; a failed call maps nothing.
    bool map_segment(std::uint64_t vaddr, const void* data, std::size_t filesz, std::size_t memsz,
                     MemPerm perm);

    // Anonymous mapping of `size` bytes (rounded up to a page) with `perm`.
    // When `hint` is non-zero and free, it is used; otherwise the allocator
    // picks the next free page at or above its cursor. Returns the base address,
    // or nullopt if the request is invalid or no space is available.
    std::optional<std::uint64_t> map_anonymous(std::size_t size, MemPerm perm,
                                               std::uint64_t hint = 0);

    // Fixed-address mapping (`MAP_FIXED` semantics): replaces any existing
    // mappings inside `[addr, addr + align_up(size))` with a fresh zero-filled
    // mapping carrying `perm`, optionally copying `init_len` bytes of `init_data`.
    bool map_fixed(std::uint64_t addr, std::size_t size, MemPerm perm,
                   const void* init_data = nullptr, std::size_t init_len = 0);

    // Map an alias of `orig_page` at `alias_page` sharing the same backing
    // storage. Used for signal delivery: 32-bit register operations zero-extend
    // `esp` to the low 4GB range, which needs to map to the same stack memory.
    bool map_page_alias(std::uint64_t orig_page, std::uint64_t alias_page) override;
    // misaligned, empty, or not fully mapped.
    bool unmap(std::uint64_t addr, std::size_t size);

    // Change the permissions of a mapped range. Returns false if the range is
    // misaligned, empty, or not fully mapped.
    bool protect(std::uint64_t addr, std::size_t size, MemPerm perm);

    // brk: the current program break (start of the heap is the initial value).
    std::uint64_t brk() const { return brk_; }
    // Set the program break. Returns the new break on success, or the
    // unchanged current break on failure (the kernel's contract).
    std::uint64_t set_brk(std::uint64_t requested);

    // Memory interface. A read/write/fetch is served only if the entire range
    // is mapped with sufficient permission; otherwise it fails.
    bool read(std::uint64_t address, std::uint8_t* out, std::size_t count) override;
    bool write(std::uint64_t address, const std::uint8_t* in, std::size_t count) override;
    bool fetch(std::uint64_t address, std::uint8_t* out, std::size_t count) override;
    std::uint64_t code_epoch() const noexcept override { return code_epoch_; }
    std::uint64_t mapping_epoch() const noexcept override { return mapping_epoch_; }

    // Invalidate translated code without changing the address-space layout.
    // The mutating operations above bump the epoch as a side effect; this is
    // for the cases where the *host* context changed rather than the guest
    // mapping -- after fork(2) the parent must not keep executing code whose
    // translation was produced before the process split.
    void bump_code_epoch() noexcept { ++code_epoch_; }
    PageAccess translate_page(std::uint64_t page_addr) noexcept override;

    // Diagnostics/tests.
    std::size_t region_count() const { return regions_.size(); }
    // Permissions covering `address`, or nullopt if unmapped. Used by tests and
    // the compatibility matrix; not on the hot path.
    std::optional<MemPerm> perm_at(std::uint64_t address) const;

private:
    struct Region {
        std::uint64_t base = 0; // page-aligned
        std::uint64_t size = 0; // multiple of kPageSize
        MemPerm perm = MemPerm::kNone;
        std::vector<std::uint8_t> bytes;
        std::uint64_t end() const { return base + size; }
    };

    // Round `value` up to a page multiple, rejecting overflow. Returns nullopt
    // on overflow.
    static std::optional<std::uint64_t> page_align_up(std::uint64_t value);

    // Index of the region containing `address`, or -1. Regions are kept sorted
    // by base and non-overlapping.
    long find_region(std::uint64_t address) const;

    // Split any region strictly spanning `boundary` into [base, boundary) and
    // [boundary, end) with identical permissions and preserved bytes.
    bool split_region_at(std::uint64_t boundary);

    // True if [addr, addr+size) is free of existing mappings and the heap reservation.
    bool range_is_free(std::uint64_t addr, std::uint64_t size) const;

    // Ensure the heap region [heap_base_, align_up(upto)) is mapped read/write,
    // growing the existing heap region (or creating it) while preserving all
    // existing heap bytes and zero-filling new pages. Returns false on OOM/cap.
    bool ensure_heap_mapped(std::uint64_t upto);

    // Copy in/out across possibly multiple regions, honouring `need` permission
    // on every region the range touches. Shared by read(), write(), and fetch().
    bool access(std::uint64_t address, std::uint8_t* buf, std::size_t count, MemPerm need,
                bool is_write);

    std::vector<Region> regions_; // sorted by base, non-overlapping
    std::uint64_t mmap_cursor_ = 0;
    std::uint64_t heap_base_ = 0;
    std::uint64_t heap_end_ = 0; // exclusive end of the reserved heap area
    std::uint64_t brk_ = 0;
    std::size_t max_map_bytes_ = kDefaultMaxMapBytes;
    std::uint64_t code_epoch_ = 1;
    // Bumped by every operation that can add, remove, resize, move or
    // re-permission a region. See Memory::mapping_epoch().
    std::uint64_t mapping_epoch_ = 1;
    mutable std::size_t mru_index_ = 0;
};

} // namespace hyperion

#endif // HYPERION_ADDRESS_SPACE_HPP
