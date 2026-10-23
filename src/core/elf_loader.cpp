// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "hyperion/elf_loader.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <unistd.h>

namespace hyperion {

namespace {

constexpr std::size_t kEhdr64Size = 64;
constexpr std::size_t kPhdr64Size = 56;
constexpr std::uint16_t kEtExec = 2;
constexpr std::uint16_t kEtDyn = 3;
constexpr std::uint16_t kEmX86_64 = 62;

constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint32_t kPtInterp = 3;
constexpr std::uint32_t kPtPhdr = 6;

constexpr std::uint32_t kPfX = 0x1;
constexpr std::uint32_t kPfW = 0x2;
constexpr std::uint32_t kPfR = 0x4;

// Linux ELF auxiliary vector tags (UAPI <linux/auxvec.h>).
constexpr std::uint64_t kAtNull = 0;
constexpr std::uint64_t kAtPhdr = 3;
constexpr std::uint64_t kAtPhent = 4;
constexpr std::uint64_t kAtPhnum = 5;
constexpr std::uint64_t kAtPagesz = 6;
constexpr std::uint64_t kAtBase = 7;
constexpr std::uint64_t kAtFlags = 8;
constexpr std::uint64_t kAtEntry = 9;
constexpr std::uint64_t kAtUid = 11;
constexpr std::uint64_t kAtEuid = 12;
constexpr std::uint64_t kAtGid = 13;
constexpr std::uint64_t kAtEgid = 14;
constexpr std::uint64_t kAtPlatform = 15;
constexpr std::uint64_t kAtHwcap = 16;
constexpr std::uint64_t kAtClktck = 17;
constexpr std::uint64_t kAtSecure = 23;
constexpr std::uint64_t kAtRandom = 25;
constexpr std::uint64_t kAtHwcap2 = 26;
constexpr std::uint64_t kAtExecfn = 31;

std::uint16_t rd16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t rd32(const std::uint8_t* p) noexcept {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    }
    return v;
}

std::uint64_t rd64(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

void wr64(std::uint8_t* p, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu);
    }
}

MemPerm elf_flags_to_perm(std::uint32_t flags) noexcept {
    MemPerm perm = MemPerm::kNone;
    if ((flags & kPfR) != 0) {
        perm = perm | MemPerm::kRead;
    }
    if ((flags & kPfW) != 0) {
        perm = perm | MemPerm::kWrite;
    }
    if ((flags & kPfX) != 0) {
        perm = perm | MemPerm::kExec;
    }
    return perm;
}

bool read_host_file(const std::string& path, std::vector<std::uint8_t>& out) {
    out.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    std::array<std::uint8_t, 8192> buf{};
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
        out.insert(out.end(), buf.data(), buf.data() + n);
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok && !out.empty();
}

} // namespace

const char* to_string(ElfLoadError error) {
    switch (error) {
    case ElfLoadError::kNone:
        return "none";
    case ElfLoadError::kNotElf:
        return "not an ELF image";
    case ElfLoadError::kTruncated:
        return "truncated ELF header or segment data";
    case ElfLoadError::kUnsupportedClass:
        return "unsupported ELF class/endianness (requires 64-bit little-endian)";
    case ElfLoadError::kUnsupportedMachine:
        return "unsupported ELF architecture (requires EM_X86_64)";
    case ElfLoadError::kUnsupportedType:
        return "unsupported ELF object type (requires ET_EXEC or ET_DYN)";
    case ElfLoadError::kNeedsInterpreter:
        return "dynamically linked ELF requires PT_INTERP runtime linker";
    case ElfLoadError::kInterpreterMissing:
        return "cannot open ELF dynamic linker (PT_INTERP); set --sysroot if cross-running";
    case ElfLoadError::kMalformedProgramHeaders:
        return "malformed ELF program header table";
    case ElfLoadError::kMappingFailed:
        return "failed to map ELF segments into guest address space";
    }
    return "unknown ELF error";
}

bool has_elf_magic(const std::uint8_t* bytes, std::size_t size) noexcept {
    return bytes != nullptr && size >= 4 && bytes[0] == 0x7Fu && bytes[1] == 'E' &&
           bytes[2] == 'L' && bytes[3] == 'F';
}

ElfInfo inspect_elf(const std::uint8_t* bytes, std::size_t size) {
    ElfInfo info;
    if (!has_elf_magic(bytes, size)) {
        info.error = ElfLoadError::kNotElf;
        return info;
    }
    if (size < kEhdr64Size) {
        info.error = ElfLoadError::kTruncated;
        return info;
    }
    if (bytes[4] != 2 || bytes[5] != 1) {
        info.error = ElfLoadError::kUnsupportedClass;
        return info;
    }

    info.type = rd16(bytes + 16);
    info.machine = rd16(bytes + 18);
    info.entry = rd64(bytes + 24);
    info.phoff = rd64(bytes + 32);
    info.phentsize = rd16(bytes + 54);
    info.phnum = rd16(bytes + 56);

    if (info.machine != kEmX86_64) {
        info.error = ElfLoadError::kUnsupportedMachine;
        return info;
    }
    if (info.type != kEtExec && info.type != kEtDyn) {
        info.error = ElfLoadError::kUnsupportedType;
        return info;
    }
    if (info.phnum == 0 || info.phentsize < kPhdr64Size) {
        info.error = ElfLoadError::kMalformedProgramHeaders;
        return info;
    }
    const std::uint64_t pht_bytes =
        static_cast<std::uint64_t>(info.phnum) * static_cast<std::uint64_t>(info.phentsize);
    if (info.phoff > size || pht_bytes > size - info.phoff) {
        info.error = ElfLoadError::kTruncated;
        return info;
    }

    info.segments.reserve(info.phnum);
    for (std::uint16_t i = 0; i < info.phnum; ++i) {
        const std::uint8_t* ph = bytes + static_cast<std::size_t>(info.phoff) +
                                 static_cast<std::size_t>(i) * info.phentsize;
        ElfSegmentInfo seg;
        seg.type = rd32(ph + 0);
        seg.flags = rd32(ph + 4);
        seg.offset = rd64(ph + 8);
        seg.vaddr = rd64(ph + 16);
        seg.filesz = rd64(ph + 32);
        seg.memsz = rd64(ph + 40);
        seg.align = rd64(ph + 48);

        if (seg.filesz > size || seg.offset > size - seg.filesz) {
            info.error = ElfLoadError::kTruncated;
            return info;
        }
        if (seg.type == kPtLoad && seg.filesz > seg.memsz) {
            info.error = ElfLoadError::kMalformedProgramHeaders;
            return info;
        }
        if (seg.type == kPtInterp) {
            info.has_interp = true;
            const auto* start = std::bit_cast<const char*>(bytes + seg.offset);
            const std::size_t max_len = static_cast<std::size_t>(seg.filesz);
            const std::size_t actual_len =
                static_cast<std::size_t>(std::find(start, start + max_len, '\0') - start);
            info.interpreter.assign(start, actual_len);
        }
        info.segments.push_back(seg);
    }
    return info;
}

std::string format_elf_info(const ElfInfo& info, const std::string& path) {
    std::string out;
    char line[256];
    std::snprintf(line, sizeof(line), "file:        %s\n", path.c_str());
    out += line;
    std::snprintf(line, sizeof(line), "format:      ELF64 little-endian x86-64 (%s)\n",
                  info.type == kEtExec ? "ET_EXEC static executable"
                                       : "ET_DYN position-independent");
    out += line;
    std::snprintf(line, sizeof(line), "entry:       0x%016llx\n",
                  static_cast<unsigned long long>(info.entry));
    out += line;
    std::snprintf(line, sizeof(line), "phdrs:       %u entries at file offset 0x%llx\n",
                  static_cast<unsigned>(info.phnum), static_cast<unsigned long long>(info.phoff));
    out += line;
    if (info.has_interp) {
        std::snprintf(line, sizeof(line), "interpreter: %s (dynamic linking supported)\n",
                      info.interpreter.c_str());
        out += line;
    } else {
        out += "linkage:     static (supported by hyperion run)\n";
    }
    std::size_t load_count = 0;
    for (const auto& seg : info.segments) {
        if (seg.type != kPtLoad) {
            continue;
        }
        ++load_count;
        std::snprintf(line, sizeof(line),
                      "  LOAD[%zu]:   vaddr=0x%016llx filesz=0x%llx memsz=0x%llx perm=%c%c%c\n",
                      load_count - 1, static_cast<unsigned long long>(seg.vaddr),
                      static_cast<unsigned long long>(seg.filesz),
                      static_cast<unsigned long long>(seg.memsz),
                      (seg.flags & kPfR) != 0 ? 'r' : '-', (seg.flags & kPfW) != 0 ? 'w' : '-',
                      (seg.flags & kPfX) != 0 ? 'x' : '-');
        out += line;
    }
    return out;
}

LoadedElf load_elf_image(const std::uint8_t* bytes, std::size_t size,
                         GuestAddressSpace& address_space, std::uint64_t pie_load_base,
                         bool allow_interp) {
    LoadedElf out;
    const ElfInfo info = inspect_elf(bytes, size);
    if (!info.ok()) {
        out.error = info.error;
        return out;
    }
    if (info.has_interp && !allow_interp) {
        out.error = ElfLoadError::kNeedsInterpreter;
        return out;
    }

    const std::uint64_t bias = (info.type == kEtDyn) ? pie_load_base : 0ULL;
    if (info.entry > std::numeric_limits<std::uint64_t>::max() - bias) {
        out.error = ElfLoadError::kMalformedProgramHeaders;
        return out;
    }

    std::map<std::uint64_t, MemPerm> page_perms;
    std::size_t mapped_load_segments = 0;
    std::uint64_t max_end = 0;

    for (const auto& seg : info.segments) {
        if (seg.type == kPtPhdr) {
            out.phdr_vaddr = bias + seg.vaddr;
            continue;
        }
        if (seg.type != kPtLoad || seg.memsz == 0) {
            continue;
        }
        ++mapped_load_segments;

        if (seg.vaddr > std::numeric_limits<std::uint64_t>::max() - bias) {
            out.error = ElfLoadError::kMalformedProgramHeaders;
            return out;
        }
        const std::uint64_t seg_start = bias + seg.vaddr;
        if (seg.memsz > std::numeric_limits<std::uint64_t>::max() - seg_start) {
            out.error = ElfLoadError::kMalformedProgramHeaders;
            return out;
        }
        const std::uint64_t seg_end = seg_start + seg.memsz;
        const std::uint64_t page_start = seg_start & ~GuestAddressSpace::kPageMask;
        if (seg_end > std::numeric_limits<std::uint64_t>::max() - GuestAddressSpace::kPageMask) {
            out.error = ElfLoadError::kMalformedProgramHeaders;
            return out;
        }
        const std::uint64_t page_end =
            (seg_end + GuestAddressSpace::kPageMask) & ~GuestAddressSpace::kPageMask;
        max_end = std::max(max_end, page_end);

        const MemPerm seg_perm = elf_flags_to_perm(seg.flags);
        for (std::uint64_t p = page_start; p < page_end; p += GuestAddressSpace::kPageSize) {
            const auto existing = page_perms.find(p);
            if (existing == page_perms.end()) {
                if (!address_space.map_image(p, nullptr, GuestAddressSpace::kPageSize,
                                             MemPerm::kRead | MemPerm::kWrite)) {
                    out.error = ElfLoadError::kMappingFailed;
                    return out;
                }
                page_perms.emplace(p, seg_perm);
            } else {
                existing->second = existing->second | seg_perm;
            }
        }

        if (seg.filesz > 0) {
            if (!address_space.write(seg_start, bytes + static_cast<std::size_t>(seg.offset),
                                     static_cast<std::size_t>(seg.filesz))) {
                out.error = ElfLoadError::kMappingFailed;
                return out;
            }
        }
        if (out.phdr_vaddr == 0 && seg.offset <= info.phoff &&
            info.phoff < seg.offset + seg.filesz) {
            out.phdr_vaddr = seg_start + (info.phoff - seg.offset);
        }
    }

    if (mapped_load_segments == 0) {
        out.error = ElfLoadError::kMalformedProgramHeaders;
        return out;
    }

    for (const auto& [page, perm] : page_perms) {
        if (!address_space.protect(page, GuestAddressSpace::kPageSize, perm)) {
            out.error = ElfLoadError::kMappingFailed;
            return out;
        }
    }

    out.entry = bias + info.entry;
    out.start_rip = out.entry;
    out.load_bias = bias;
    out.phentsize = info.phentsize;
    out.phnum = info.phnum;
    out.brk_hint = max_end;
    return out;
}

LoadedElf load_linux_elf_process(const std::uint8_t* bytes, std::size_t size,
                                 GuestAddressSpace& address_space, GuestState& state,
                                 const std::vector<std::string>& argv,
                                 const std::vector<std::string>& envp, const std::string& sysroot,
                                 std::size_t stack_bytes, const std::string& image_path) {
    const ElfInfo exe_info = inspect_elf(bytes, size);
    if (!exe_info.ok()) {
        LoadedElf err;
        err.error = exe_info.error;
        return err;
    }

    constexpr std::uint64_t kPieExeBase = 0x555555554000ULL;
    constexpr std::uint64_t kInterpBase = 0x7ffff7fc0000ULL;

    LoadedElf loaded =
        load_elf_image(bytes, size, address_space, kPieExeBase, /*allow_interp=*/true);
    if (!loaded.ok()) {
        return loaded;
    }

    if (exe_info.has_interp) {
        std::vector<std::uint8_t> interp_bytes;
        bool opened = false;
        if (!sysroot.empty()) {
            opened = read_host_file(sysroot + exe_info.interpreter, interp_bytes);
        }
        if (!opened) {
            opened = read_host_file(exe_info.interpreter, interp_bytes);
        }
        if (!opened) {
            loaded.error = ElfLoadError::kInterpreterMissing;
            return loaded;
        }
        const LoadedElf interp = load_elf_image(interp_bytes.data(), interp_bytes.size(),
                                                address_space, kInterpBase, /*allow_interp=*/true);
        if (!interp.ok()) {
            loaded.error = interp.error;
            return loaded;
        }
        loaded.interp_base = interp.load_bias;
        loaded.start_rip = interp.entry;
    }

    const std::vector<std::string> effective_argv =
        argv.empty() ? std::vector<std::string>{"hyperion-guest"} : argv;
    const std::uint64_t rsp = setup_linux_x86_64_stack(
        address_space, effective_argv, &loaded, 0x7fffffffe000ULL, stack_bytes, envp, image_path);
    if (rsp == 0) {
        loaded.error = ElfLoadError::kMappingFailed;
        return loaded;
    }

    state = GuestState{};
    state.rip = loaded.start_rip;
    state.reg(Gpr::kRsp) = rsp;
    state.reg(Gpr::kRdx) = 0; // System V AMD64 ABI §3.4.1: %rdx = rtld_fini (NULL at kernel entry)
    return loaded;
}

std::uint64_t setup_linux_x86_64_stack(GuestAddressSpace& address_space,
                                       const std::vector<std::string>& argv,
                                       const LoadedElf* loaded_elf, std::uint64_t stack_top,
                                       std::size_t stack_bytes,
                                       const std::vector<std::string>& envp,
                                       const std::string& image_path) {
    if (stack_bytes < GuestAddressSpace::kPageSize ||
        (stack_top & GuestAddressSpace::kPageMask) != 0 || stack_top <= stack_bytes) {
        return 0;
    }
    const std::uint64_t stack_base = stack_top - stack_bytes;
    const auto mapped =
        address_space.map_anonymous(stack_bytes, MemPerm::kRead | MemPerm::kWrite, stack_base);
    if (!mapped || *mapped != stack_base) {
        return 0;
    }

    std::uint64_t cursor = stack_top;

    // 16 random seed bytes for AT_RANDOM.
    cursor -= 16;
    const std::uint64_t random_addr = cursor;
    static constexpr std::array<std::uint8_t, 16> kSeed = {
        0x48, 0x59, 0x50, 0x45, 0x52, 0x49, 0x4F, 0x4E,
        0xDB, 0x70, 0x20, 0x26, 0xAA, 0x64, 0x86, 0x64,
    };
    if (!address_space.write(random_addr, kSeed.data(), kSeed.size())) {
        return 0;
    }

    // "x86_64\0" string for AT_PLATFORM.
    static constexpr std::array<std::uint8_t, 7> kPlatform = {'x', '8', '6', '_', '6', '4', '\0'};
    cursor -= kPlatform.size();
    const std::uint64_t platform_addr = cursor;
    if (!address_space.write(platform_addr, kPlatform.data(), kPlatform.size())) {
        return 0;
    }

    const auto push_str = [&](const std::string& s) -> std::uint64_t {
        const std::size_t len = s.size() + 1;
        if (cursor - stack_base < len + 512) {
            return 0;
        }
        cursor -= len;
        if (!address_space.write(cursor, std::bit_cast<const std::uint8_t*>(s.c_str()), len)) {
            return 0;
        }
        return cursor;
    };

    std::vector<std::uint64_t> env_ptrs;
    env_ptrs.reserve(envp.size());
    for (const std::string& e : envp) {
        const std::uint64_t p = push_str(e);
        if (p == 0) {
            return 0;
        }
        env_ptrs.push_back(p);
    }

    std::vector<std::uint64_t> argv_ptrs;
    argv_ptrs.reserve(argv.size());
    for (const std::string& s : argv) {
        const std::uint64_t p = push_str(s);
        if (p == 0) {
            return 0;
        }
        argv_ptrs.push_back(p);
    }

    cursor &= ~std::uint64_t{0xF};

    // Baseline x86-64 CPUID EDX HWCAP (FPU, TSC, MSR, PAE, CMOV, MMX, FXSR, SSE, SSE2 =
    // 0x078bfbfd).
    constexpr std::uint64_t kBaselineX86Hwcap = 0x078bfbfdULL;

    std::vector<std::pair<std::uint64_t, std::uint64_t>> auxv;
    auxv.emplace_back(kAtPagesz, GuestAddressSpace::kPageSize);
    auxv.emplace_back(kAtClktck, 100ULL);
    auxv.emplace_back(kAtFlags, 0ULL);
    if (loaded_elf != nullptr) {
        if (loaded_elf->phdr_vaddr != 0) {
            auxv.emplace_back(kAtPhdr, loaded_elf->phdr_vaddr);
        }
        auxv.emplace_back(kAtPhent, loaded_elf->phentsize);
        auxv.emplace_back(kAtPhnum, loaded_elf->phnum);
        auxv.emplace_back(kAtBase, loaded_elf->interp_base);
        auxv.emplace_back(kAtEntry, loaded_elf->entry);
    }
    auxv.emplace_back(kAtUid, static_cast<std::uint64_t>(::getuid()));
    auxv.emplace_back(kAtEuid, static_cast<std::uint64_t>(::geteuid()));
    auxv.emplace_back(kAtGid, static_cast<std::uint64_t>(::getgid()));
    auxv.emplace_back(kAtEgid, static_cast<std::uint64_t>(::getegid()));
    auxv.emplace_back(kAtSecure, 0ULL);
    auxv.emplace_back(kAtHwcap, kBaselineX86Hwcap);
    auxv.emplace_back(kAtHwcap2, 0ULL);
    auxv.emplace_back(kAtPlatform, platform_addr);
    // AT_EXECFN is the path of the file that was executed, which the kernel
    // records as an absolute path. It is NOT argv[0]: for `hyperion run ./prog`
    // argv[0] is relative, and glibc's _dl_get_origin() aborts the process when
    // AT_EXECFN and readlink("/proc/self/exe") disagree in shape. Fall back to
    // argv[0] only when the caller could not supply a path at all.
    const std::string& execfn =
        !image_path.empty() ? image_path : (!argv.empty() ? argv.front() : std::string{});
    const std::uint64_t execfn_addr = execfn.empty() ? 0 : push_str(execfn);
    if (execfn_addr != 0) {
        auxv.emplace_back(kAtExecfn, execfn_addr);
    }
    auxv.emplace_back(kAtRandom, random_addr);
    auxv.emplace_back(kAtNull, 0ULL);

    const std::size_t total_qwords =
        1 + argv_ptrs.size() + 1 + env_ptrs.size() + 1 + 2 * auxv.size();
    // The SysV AMD64 ABI requires %rsp to be 16-byte aligned at process entry.
    // Re-align here rather than assuming the string pushes above left `cursor`
    // aligned: they move it by each string's length, so any change to what goes
    // on the stack (an extra auxiliary string, a longer environment) shifts it.
    // Aligning down is always safe -- it only moves the stack further from the
    // strings that were just written.
    cursor &= ~std::uint64_t{0xF};
    if ((total_qwords & 1u) != 0) {
        cursor -= 8;
    }
    const std::size_t frame_bytes = total_qwords * 8;
    if (cursor - stack_base < frame_bytes) {
        return 0;
    }
    const std::uint64_t initial_rsp = cursor - frame_bytes;
    std::vector<std::uint8_t> frame(frame_bytes, 0);
    std::size_t slot = 0;
    wr64(frame.data() + 8 * slot++, static_cast<std::uint64_t>(argv_ptrs.size()));
    for (const std::uint64_t p : argv_ptrs) {
        wr64(frame.data() + 8 * slot++, p);
    }
    wr64(frame.data() + 8 * slot++, 0ULL);
    for (const std::uint64_t p : env_ptrs) {
        wr64(frame.data() + 8 * slot++, p);
    }
    wr64(frame.data() + 8 * slot++, 0ULL);
    for (const auto& [tag, val] : auxv) {
        wr64(frame.data() + 8 * slot++, tag);
        wr64(frame.data() + 8 * slot++, val);
    }
    if (!address_space.write(initial_rsp, frame.data(), frame.size())) {
        return 0;
    }
    return initial_rsp;
}

} // namespace hyperion
