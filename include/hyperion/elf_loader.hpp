// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// x86-64 Linux ELF64 executable inspector, static + dynamic (`PT_INTERP`)
// loader, and System V AMD64 initial process stack builder.
#ifndef HYPERION_ELF_LOADER_HPP
#define HYPERION_ELF_LOADER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hyperion/address_space.hpp"
#include "hyperion/guest_state.hpp"

namespace hyperion {

enum class ElfLoadError : std::uint8_t {
    kNone = 0,
    kNotElf,
    kTruncated,
    kUnsupportedClass,   // not ELFCLASS64 or not little-endian
    kUnsupportedMachine, // not EM_X86_64 (62)
    kUnsupportedType,    // neither ET_EXEC (2) nor ET_DYN (3)
    kNeedsInterpreter,   // PT_INTERP present when calling static-only `load_elf_image`
    kInterpreterMissing, // PT_INTERP file not found on disk / sysroot
    kMalformedProgramHeaders,
    kMappingFailed,
};

const char* to_string(ElfLoadError error);

struct ElfSegmentInfo {
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    std::uint64_t offset = 0;
    std::uint64_t vaddr = 0;
    std::uint64_t filesz = 0;
    std::uint64_t memsz = 0;
    std::uint64_t align = 0;
};

struct ElfInfo {
    ElfLoadError error = ElfLoadError::kNone;
    std::uint16_t type = 0;    // ET_EXEC (2) or ET_DYN (3)
    std::uint16_t machine = 0; // EM_X86_64 (62)
    std::uint64_t entry = 0;
    std::uint64_t phoff = 0;
    std::uint16_t phentsize = 0;
    std::uint16_t phnum = 0;
    bool has_interp = false;
    std::string interpreter;
    std::vector<ElfSegmentInfo> segments;

    bool ok() const noexcept { return error == ElfLoadError::kNone; }
};

struct LoadedElf {
    ElfLoadError error = ElfLoadError::kNone;
    std::uint64_t entry = 0;       // executable entry point (`AT_ENTRY`)
    std::uint64_t start_rip = 0;   // initial CPU `%rip` (`interp_entry` when dynamic, else `entry`)
    std::uint64_t interp_base = 0; // `AT_BASE` (`0` when static)
    std::uint64_t load_bias = 0;
    std::uint64_t phdr_vaddr = 0;
    std::uint16_t phentsize = 0;
    std::uint16_t phnum = 0;
    std::uint64_t brk_hint = 0;

    bool ok() const noexcept { return error == ElfLoadError::kNone; }
};

// Returns true if `bytes` starts with the 4-byte `\x7fELF` magic.
bool has_elf_magic(const std::uint8_t* bytes, std::size_t size) noexcept;

// Parse and validate an ELF64 x86-64 executable or shared-object image.
ElfInfo inspect_elf(const std::uint8_t* bytes, std::size_t size);

// Format human-readable summary of `info` for `hyperion info <path>`.
std::string format_elf_info(const ElfInfo& info, const std::string& path);

// Map a static `ET_EXEC` or static-PIE `ET_DYN` image into `address_space`.
LoadedElf load_elf_image(const std::uint8_t* bytes, std::size_t size,
                         GuestAddressSpace& address_space,
                         std::uint64_t pie_load_base = 0x400000ULL, bool allow_interp = false);

// Full Box64-style process loader: loads static `ET_EXEC` / `ET_DYN` images as
// well as dynamically linked ELF64 executables (`PT_INTERP`, resolved via
// `sysroot + interp` first and host `interp` second), populates the complete
// System V AMD64 stack (`argc`, `argv`, `envp`, kernel `auxv`), and initializes
// `state.rip` + `state.rsp`.
LoadedElf load_linux_elf_process(const std::uint8_t* bytes, std::size_t size,
                                 GuestAddressSpace& address_space, GuestState& state,
                                 const std::vector<std::string>& argv,
                                 const std::vector<std::string>& envp = {},
                                 const std::string& sysroot = {},
                                 std::size_t stack_bytes = 8ULL << 20,
                                 const std::string& image_path = {});

// Map a read/write guest stack and lay out the System V AMD64 initial process
// stack frame (`argc`, `argv[]`, `NULL`, `envp[]`, `NULL`, `auxv[]`), returning
// the 16-byte-aligned initial `%rsp` value (or 0 on failure).
//
// `image_path` becomes the `AT_EXECFN` auxiliary vector entry. The kernel fills
// AT_EXECFN with the *absolute* path of the file it executed, not with argv[0],
// and glibc relies on that: `_dl_get_origin()` compares it against
// readlink("/proc/self/exe") and aborts on a mismatch in shape. Passing argv[0]
// here -- which for `hyperion run ./prog` is relative -- makes a statically
// linked glibc guest abort during startup, before its main() runs.
std::uint64_t setup_linux_x86_64_stack(GuestAddressSpace& address_space,
                                       const std::vector<std::string>& argv,
                                       const LoadedElf* loaded_elf = nullptr,
                                       std::uint64_t stack_top = 0x7fffffffe000ULL,
                                       std::size_t stack_bytes = 64ULL * 1024ULL,
                                       const std::vector<std::string>& envp = {},
                                       const std::string& image_path = {});

} // namespace hyperion

#endif // HYPERION_ELF_LOADER_HPP
