// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest address space tests (Phase 6, increment 2).
//
// These verify the security-relevant behaviour of GuestAddressSpace: that every
// access is bounds- and permission-checked, that ranges cannot wrap around the
// address space, and that map/unmap/protect/brk follow the kernel's contract.
// The class is exercised through the same Memory interface the interpreter and
// translator use, so the tests also serve as an integration check.
#include <cstdint>
#include <cstring>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/address_space.hpp"
#include "hyperion/exec_mem.hpp"
#include "hyperion/translation_cache.hpp"

using hyperion::GuestAddressSpace;
using hyperion::MemPerm;

namespace {

constexpr std::uint64_t kPage = GuestAddressSpace::kPageSize;

} // namespace

HYPERION_TEST("address_space", "map_image_is_readable_and_reports_its_permissions") {
    GuestAddressSpace as;
    const std::uint8_t image[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    HYPERION_CHECK(as.map_image(0x400000, image, sizeof(image), MemPerm::kRead | MemPerm::kExec));

    std::uint8_t out[4] = {};
    HYPERION_CHECK(as.read(0x400000, out, sizeof(out)));
    HYPERION_CHECK(out[0] == 0xDE && out[3] == 0xEF);

    const auto perm = as.perm_at(0x400000);
    HYPERION_CHECK(perm.has_value());
    HYPERION_CHECK(hyperion::has_perm(*perm, MemPerm::kRead));
    HYPERION_CHECK(hyperion::has_perm(*perm, MemPerm::kExec));
    HYPERION_CHECK(!hyperion::has_perm(*perm, MemPerm::kWrite));
}

HYPERION_TEST("address_space", "write_to_read_only_mapping_fails") {
    GuestAddressSpace as;
    const std::uint8_t image[4] = {1, 2, 3, 4};
    HYPERION_CHECK(as.map_image(0x400000, image, sizeof(image), MemPerm::kRead));
    const std::uint8_t value = 0xFF;
    HYPERION_CHECK(!as.write(0x400000, &value, 1)); // read-only: must fault
    std::uint8_t out = 0;
    HYPERION_CHECK(as.read(0x400000, &out, 1)); // still readable
    HYPERION_CHECK(out == 1);
}

HYPERION_TEST("address_space", "read_past_the_end_of_a_mapping_faults") {
    GuestAddressSpace as;
    const std::uint8_t image[4] = {1, 2, 3, 4};
    HYPERION_CHECK(as.map_image(0x400000, image, sizeof(image), MemPerm::kRead));
    std::uint8_t out[4] = {};
    // The image occupies one page at 0x400000; a read starting inside it but
    // crossing the page boundary must fail rather than return neighbouring
    // bytes.
    HYPERION_CHECK(!as.read(0x400000 + kPage - 2, out, 4));
    // A read entirely within the page succeeds.
    HYPERION_CHECK(as.read(0x400000, out, 4));
}

HYPERION_TEST("address_space", "unmapped_read_and_write_fault") {
    GuestAddressSpace as;
    std::uint8_t buf[8] = {};
    HYPERION_CHECK(!as.read(0x1000, buf, sizeof(buf)));
    HYPERION_CHECK(!as.write(0x1000, buf, sizeof(buf)));
}

HYPERION_TEST("address_space", "anonymous_mapping_is_writable_and_disjoint") {
    GuestAddressSpace as;
    const auto a = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite);
    const auto b = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite);
    HYPERION_CHECK(a.has_value());
    HYPERION_CHECK(b.has_value());
    HYPERION_CHECK(*a != *b);
    HYPERION_CHECK(*b >= *a + kPage || *a >= *b + kPage); // no overlap

    const std::uint8_t value = 0x42;
    HYPERION_CHECK(as.write(*a, &value, 1));
    std::uint8_t out = 0;
    HYPERION_CHECK(as.read(*a, &out, 1));
    HYPERION_CHECK(out == 0x42);
}

HYPERION_TEST("address_space", "anonymous_mapping_honours_a_free_hint") {
    GuestAddressSpace as;
    const std::uint64_t hint = 0x600000;
    const auto got = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite, hint);
    HYPERION_CHECK(got.has_value());
    HYPERION_CHECK(*got == hint);

    // A second request at the same hint cannot use it and must fall back.
    const auto second = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite, hint);
    HYPERION_CHECK(second.has_value());
    HYPERION_CHECK(*second != hint);
}

HYPERION_TEST("address_space", "mprotect_changes_access") {
    GuestAddressSpace as;
    const auto addr = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite);
    HYPERION_CHECK(addr.has_value());
    const std::uint8_t v = 7;
    HYPERION_CHECK(as.write(*addr, &v, 1));

    HYPERION_CHECK(as.protect(*addr, kPage, MemPerm::kRead)); // drop write
    HYPERION_CHECK(!as.write(*addr, &v, 1));

    HYPERION_CHECK(as.protect(*addr, kPage, MemPerm::kNone)); // PROT_NONE
    std::uint8_t out = 0;
    HYPERION_CHECK(!as.read(*addr, &out, 1));

    HYPERION_CHECK(as.protect(*addr, kPage, MemPerm::kRead | MemPerm::kWrite)); // restore
    HYPERION_CHECK(as.read(*addr, &out, 1));
    HYPERION_CHECK(out == 7); // contents survive a permission change
}

HYPERION_TEST("address_space", "mprotect_on_an_unmapped_range_fails") {
    GuestAddressSpace as;
    HYPERION_CHECK(!as.protect(0x900000, kPage, MemPerm::kRead));
}

HYPERION_TEST("address_space", "unmap_removes_the_mapping") {
    GuestAddressSpace as;
    const auto addr = as.map_anonymous(2 * kPage, MemPerm::kRead | MemPerm::kWrite);
    HYPERION_CHECK(addr.has_value());
    const std::uint8_t v = 1;
    HYPERION_CHECK(as.write(*addr, &v, 1));

    HYPERION_CHECK(as.unmap(*addr, 2 * kPage));
    std::uint8_t out = 0;
    HYPERION_CHECK(!as.read(*addr, &out, 1));
    HYPERION_CHECK(!as.perm_at(*addr).has_value());
}

HYPERION_TEST("address_space", "unmap_splits_regions_and_rejects_unmapped_ranges") {
    GuestAddressSpace as;
    // Map a 3-page region [a, a + 3*kPage), write distinct markers on pages 0, 1, 2,
    // punch a hole in the middle page [a + kPage, a + 2*kPage) (matching the
    // ELF dynamic loader guard-page pattern), and verify pages 0 and 2 preserve
    // their exact bytes while page 1 faults and a second unmap on the hole fails.
    const auto addr = as.map_anonymous(3 * kPage, MemPerm::kRead | MemPerm::kWrite);
    HYPERION_CHECK(addr.has_value());
    const std::uint8_t p0 = 0xAA;
    const std::uint8_t p1 = 0xBB;
    const std::uint8_t p2 = 0xCC;
    HYPERION_CHECK(as.write(*addr, &p0, 1));
    HYPERION_CHECK(as.write(*addr + kPage, &p1, 1));
    HYPERION_CHECK(as.write(*addr + 2 * kPage, &p2, 1));

    HYPERION_CHECK(as.unmap(*addr + kPage, kPage));
    std::uint8_t out = 0;
    HYPERION_CHECK(as.read(*addr, &out, 1) && out == 0xAA);
    HYPERION_CHECK(!as.read(*addr + kPage, &out, 1));
    HYPERION_CHECK(as.read(*addr + 2 * kPage, &out, 1) && out == 0xCC);

    // Unmapping an unmapped range fails cleanly.
    HYPERION_CHECK(!as.unmap(*addr + kPage, kPage));
    HYPERION_CHECK(!as.unmap(0x900000, kPage));
}

HYPERION_TEST("address_space", "overlapping_map_is_rejected") {
    GuestAddressSpace as;
    HYPERION_CHECK(as.map_image(0x400000, nullptr, kPage, MemPerm::kRead));
    HYPERION_CHECK(!as.map_image(0x400000, nullptr, kPage, MemPerm::kRead));
    HYPERION_CHECK(!as.map_image(0x400800, nullptr, kPage, MemPerm::kRead)); // straddles
}

HYPERION_TEST("address_space", "misaligned_requests_are_rejected") {
    GuestAddressSpace as;
    HYPERION_CHECK(!as.map_image(0x400001, nullptr, 16, MemPerm::kRead)); // not page-aligned
    HYPERION_CHECK(!as.unmap(1, kPage));
    HYPERION_CHECK(!as.protect(1, kPage, MemPerm::kRead));
}

HYPERION_TEST("address_space", "a_huge_size_does_not_wrap") {
    GuestAddressSpace as;
    // A guest-supplied size near UINT64_MAX must not wrap the end calculation
    // into a valid-looking range.
    std::uint8_t buf[8] = {};
    HYPERION_CHECK(!as.read(UINT64_MAX - 3, buf, 8));
    HYPERION_CHECK(!as.write(UINT64_MAX - 3, buf, 8));
    HYPERION_CHECK(!as.map_anonymous(SIZE_MAX, MemPerm::kRead));
}

HYPERION_TEST("address_space", "brk_grows_and_shrinks_within_bounds") {
    GuestAddressSpace as(0x7f0000000000ULL, 0x200000ULL, 1ULL << 20);
    HYPERION_CHECK(as.brk() == 0x200000ULL);

    // Growing within the heap area succeeds and the new memory is writable.
    HYPERION_CHECK(as.set_brk(0x201000) == 0x201000);
    const std::uint8_t v = 0x5A;
    HYPERION_CHECK(as.write(0x200FF0, &v, 1));

    // Shrinking is allowed.
    HYPERION_CHECK(as.set_brk(0x200800) == 0x200800);

    // A request outside the reserved heap area leaves brk unchanged. The heap
    // area is [0x200000, 0x300000) here.
    HYPERION_CHECK(as.set_brk(0x400000) == 0x200800);
    HYPERION_CHECK(as.set_brk(0x100000) == 0x200800);
}

HYPERION_TEST("address_space", "a_read_spanning_two_regions_succeeds_when_both_allow_it") {
    // Adjacent anonymous mappings: a read crossing the boundary must stitch
    // them together, not fail.
    GuestAddressSpace as;
    const auto a = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite, 0x100000);
    HYPERION_CHECK(a.has_value());
    const auto b = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite, 0x101000);
    HYPERION_CHECK(b.has_value());
    HYPERION_CHECK(*b == *a + kPage);

    const std::uint8_t tail = 0x11;
    const std::uint8_t head = 0x22;
    HYPERION_CHECK(as.write(*a + kPage - 1, &tail, 1));
    HYPERION_CHECK(as.write(*b, &head, 1));

    std::uint8_t out[2] = {};
    HYPERION_CHECK(as.read(*a + kPage - 1, out, 2));
    HYPERION_CHECK(out[0] == 0x11 && out[1] == 0x22);
}

HYPERION_TEST("address_space", "a_read_spanning_a_region_without_permission_fails") {
    GuestAddressSpace as;
    const auto a = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite, 0x100000);
    HYPERION_CHECK(a.has_value());
    const auto b = as.map_anonymous(kPage, MemPerm::kNone, 0x101000); // PROT_NONE
    HYPERION_CHECK(b.has_value());

    std::uint8_t out[2] = {};
    HYPERION_CHECK(!as.read(*a + kPage - 1, out, 2)); // crosses into PROT_NONE
}

HYPERION_TEST("address_space", "an_oversized_anonymous_mapping_is_refused") {
    // The backing store is host memory. A single mapping larger than the cap
    // must be refused (nullopt -> ENOMEM) rather than driving an unbounded host
    // allocation. Uses a tiny cap so the test never allocates anything large.
    GuestAddressSpace as(/*mmap_base=*/0x7f0000000000ULL, /*heap_base=*/0x200000ULL,
                         /*heap_capacity=*/1ULL << 20, /*max_map_bytes=*/64ULL * 1024ULL);
    HYPERION_CHECK(
        !as.map_anonymous(1024ULL * 1024ULL, MemPerm::kRead | MemPerm::kWrite).has_value());
    // A request at or below the cap still succeeds.
    HYPERION_CHECK(as.map_anonymous(64ULL * 1024ULL, MemPerm::kRead | MemPerm::kWrite).has_value());
}

HYPERION_TEST("address_space", "brk_page_growth_preserves_existing_heap_bytes_and_honours_cap") {
    // Critical regression: ensure_heap_mapped() previously replaced `heap.bytes`
    // with a fresh zero-filled std::vector whenever brk crossed into a new page,
    // silently wiping every byte the guest had already allocated on the heap!
    GuestAddressSpace as(/*mmap_base=*/0x7f0000000000ULL, /*heap_base=*/0x200000ULL,
                         /*heap_capacity=*/64ULL * 1024ULL, /*max_map_bytes=*/8ULL * 1024ULL);
    HYPERION_CHECK_EQ(as.set_brk(0x200900ULL), 0x200900ULL);
    const std::uint8_t magic[4] = {0x48, 0x59, 0x50, 0x21};
    HYPERION_CHECK(as.write(0x200010ULL, magic, sizeof(magic)));

    // Grow across a page boundary into page 2: earlier heap data MUST survive!
    HYPERION_CHECK_EQ(as.set_brk(0x201800ULL), 0x201800ULL);
    std::uint8_t got[4] = {};
    HYPERION_CHECK(as.read(0x200010ULL, got, sizeof(got)));
    HYPERION_CHECK(std::memcmp(got, magic, sizeof(magic)) == 0);

    // Growing past max_map_bytes (8 KiB) must return unchanged brk on failure.
    HYPERION_CHECK_EQ(as.set_brk(0x203000ULL), 0x201800ULL);
}

HYPERION_TEST("address_space", "sub_region_mprotect_and_nx_instruction_fetch_enforcement") {
    GuestAddressSpace as;
    // Map 2 pages RX with code on page 0 and data on page 1, then mprotect ONLY
    // page 1 to RW: page 0 MUST remain RX (executable, non-writable) while page 1
    // becomes RW (non-executable, writable)!
    const std::uint8_t program[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xF4}; // mov eax, 42 ; hlt
    const auto base = as.map_anonymous(2 * kPage, MemPerm::kRead | MemPerm::kExec, 0x500000ULL);
    HYPERION_CHECK(base.has_value());
    // Populate page 0 before locking permissions via protect.
    HYPERION_CHECK(as.protect(*base, 2 * kPage, MemPerm::kRead | MemPerm::kWrite));
    HYPERION_CHECK(as.write(*base, program, sizeof(program)));
    HYPERION_CHECK(as.write(*base + kPage, program, sizeof(program)));
    HYPERION_CHECK(as.protect(*base, 2 * kPage, MemPerm::kRead | MemPerm::kExec));

    // Sub-region mprotect on page 1 ONLY -> RW (NX).
    HYPERION_CHECK(as.protect(*base + kPage, kPage, MemPerm::kRead | MemPerm::kWrite));
    HYPERION_CHECK(as.perm_at(*base) == (MemPerm::kRead | MemPerm::kExec));
    HYPERION_CHECK(as.perm_at(*base + kPage) == (MemPerm::kRead | MemPerm::kWrite));

    // Executing from executable page 0 succeeds in Tier 0 (and Tier 1 on AArch64).
    hyperion::GuestState ok_state;
    ok_state.rip = *base;
    const auto ok0 = hyperion::run_program(as, ok_state);
    HYPERION_CHECK(ok0.status == hyperion::RunStatus::kOk);
    HYPERION_CHECK_EQ(ok0.state.reg(hyperion::Gpr::kRax), 42ULL);

    // Executing from NX page 1 MUST FAULT in Tier 0!
    hyperion::GuestState nx_state;
    nx_state.rip = *base + kPage;
    const auto nx0 = hyperion::run_program(as, nx_state);
    HYPERION_CHECK(nx0.status == hyperion::RunStatus::kFault);

    if (hyperion::ExecMem::executable_supported()) {
        hyperion::TranslationCache cache;
        const auto ok1 = cache.run(as, ok_state);
        HYPERION_CHECK(ok1.status == hyperion::RunStatus::kOk);
        HYPERION_CHECK_EQ(ok1.state.reg(hyperion::Gpr::kRax), 42ULL);

        const auto nx1 = cache.run(as, nx_state);
        HYPERION_CHECK(nx1.status == hyperion::RunStatus::kFault);

        // Even when page 0 WAS ALREADY CACHED in Tier 1, dropping PROT_EXEC via
        // protect() bumps code_epoch() and invalidates the stale translation!
        HYPERION_CHECK(as.protect(*base, kPage, MemPerm::kRead));
        const auto revoked = cache.run(as, ok_state);
        HYPERION_CHECK(revoked.status == hyperion::RunStatus::kFault);
    }
}

// --- mapping generation: the contract the data SoftTlb validates against ----
//
// The interpreter and the JIT both cache `host_page` pointers in a SoftTlb and
// re-validate them against mapping_epoch(). That is only sound if *every*
// operation that can add, remove, resize, move or re-permission a page bumps it.
// code_epoch() is not a substitute: it has to track self-modifying code, so it
// legitimately bumps only when executable memory is involved, and a plain
// non-exec mprotect(RELRO) or mmap moves backing stores without touching code.
//
// This test walks each mutating operation and requires the generation to move.
// It would have caught the bug this pair of counters was introduced to fix: with
// only code_epoch, a non-exec protect() left the data TLB holding pointers into a
// buffer that split_region_at() had already replaced, and the guest read freed
// memory.
HYPERION_TEST("address_space", "every_mapping_mutation_bumps_the_mapping_generation") {
    GuestAddressSpace as;

    const auto bump_on = [&as](const char* what, auto&& op) {
        const std::uint64_t before = as.mapping_epoch();
        op();
        if (as.mapping_epoch() == before) {
            std::fprintf(stderr, "  mapping_epoch did not move for: %s\n", what);
        }
        HYPERION_CHECK(as.mapping_epoch() != before);
    };

    std::uint64_t one = 0;
    bump_on("map_anonymous", [&] {
        const auto base = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite);
        HYPERION_CHECK(base.has_value());
        one = base.value_or(0);
    });

    // Non-executable protect: the exact case code_epoch() deliberately ignores,
    // because re-permissioning a page is not self-modifying code.
    bump_on("protect(non-exec)", [&] { HYPERION_CHECK(as.protect(one, kPage, MemPerm::kRead)); });

    // A protect that splits a mapping: split_region_at() replaces the tail's
    // backing store, so host pointers cached for the tail pages go stale even
    // though the split pages keep their original permissions.
    std::uint64_t split_base = 0;
    bump_on("map_anonymous(4 pages)", [&] {
        const auto base = as.map_anonymous(4 * kPage, MemPerm::kRead | MemPerm::kWrite);
        HYPERION_CHECK(base.has_value());
        split_base = base.value_or(0);
    });
    bump_on("protect(split)", [&] {
        HYPERION_CHECK(as.protect(split_base + kPage, kPage, MemPerm::kRead | MemPerm::kWrite));
    });

    bump_on("unmap", [&] { HYPERION_CHECK(as.unmap(split_base, kPage)); });

    bump_on("map_fixed", [&] {
        HYPERION_CHECK(
            as.map_fixed(split_base, kPage, MemPerm::kRead | MemPerm::kWrite, nullptr, 0));
    });

    // brk changes the heap's backing store shape with no mprotect at all.
    const std::uint64_t grown = as.set_brk(as.brk() + 64 * kPage);
    HYPERION_CHECK_EQ(grown, as.brk());
    bump_on("set_brk(grow)", [&] { (void)as.set_brk(grown + 16 * kPage); });
    bump_on("set_brk(shrink)", [&] { (void)as.set_brk(grown); });

    bump_on("reset", [&] { as.reset(); });

    // The two generations must stay independent: an ordinary non-exec mapping
    // change has to be visible to the TLB yet must not be mistaken for
    // self-modifying code, or every mmap would discard the whole translation
    // cache.
    GuestAddressSpace plain;
    const std::uint64_t code_before = plain.code_epoch();
    const std::uint64_t mapping_before = plain.mapping_epoch();
    HYPERION_CHECK(plain.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite).has_value());
    HYPERION_CHECK_EQ(plain.code_epoch(), code_before);
    HYPERION_CHECK(plain.mapping_epoch() != mapping_before);
}
