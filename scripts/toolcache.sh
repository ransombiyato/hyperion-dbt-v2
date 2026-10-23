#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# Repo-local tool cache.
#
# The development container does NOT persist /usr between sessions, but the
# repository directory does. Re-running scripts/bootstrap.sh after every reset
# is slow and depends on the package mirror, so the tools the cross-validation
# pipeline needs are copied into tools/cache/ (git-ignored, so binaries never
# enter git) and reused from there when present.
#
# Cached by default:
#   bin/qemu-aarch64-static   the user-mode AArch64 emulator used to execute
#                             Hyperion's generated AArch64 code on an x86-64 host
#
# Optionally cached with --with-toolchain (much larger, ~150 MB):
#   toolchain/*.tar.gz        the AArch64 cross compiler, its internal libs and
#                             the /usr/aarch64-linux-gnu sysroot
#
# Usage:
#   scripts/toolcache.sh populate [--with-toolchain]
#   scripts/toolcache.sh restore        # reinstate a cached toolchain after a reset
#   scripts/toolcache.sh status
#   scripts/toolcache.sh env            # print export lines for the current shell
#
# Environment:
#   HYPERION_TOOLCACHE   override the cache directory (default tools/cache)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cache="${HYPERION_TOOLCACHE:-${root}/tools/cache}"
bin_dir="${cache}/bin"
tc_dir="${cache}/toolchain"

SUDO=""
if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
fi

# The tools we prefer to run from the cache. Each is a "system name:cache name"
# pair; a cached copy always wins so a session reset cannot change which
# compiler or emulator a run used.
qemu_system="${HYPERION_QEMU:-qemu-aarch64}"
qemu_cached="${bin_dir}/qemu-aarch64-static"

die() {
    echo "toolcache: $*" >&2
    exit 1
}

# A statically linked qemu is preferred because it needs no interpreter or
# shared libraries, which is exactly what makes it survive a container reset.
cache_qemu() {
    mkdir -p "${bin_dir}"
    # A cached copy is authoritative and survives a reset that wipes /usr; if it
    # is already here there is nothing to re-cache.
    if [ -x "${qemu_cached}" ]; then
        echo "  qemu already cached at ${qemu_cached}; leaving it in place"
        return 0
    fi
    local src=""
    if command -v qemu-aarch64-static >/dev/null 2>&1; then
        src="$(command -v qemu-aarch64-static)"
    elif [ -x /usr/bin/qemu-aarch64-static ]; then
        src=/usr/bin/qemu-aarch64-static
    elif command -v "${qemu_system}" >/dev/null 2>&1; then
        src="$(command -v "${qemu_system}")"
    fi
    [ -n "${src}" ] || die "no qemu-aarch64-static or ${qemu_system} found; run scripts/bootstrap.sh first"

    cp -f "${src}" "${qemu_cached}.tmp"
    chmod 0755 "${qemu_cached}.tmp"
    mv -f "${qemu_cached}.tmp" "${qemu_cached}"
    echo "  cached ${qemu_cached}  ($(du -h "${qemu_cached}" | cut -f1), from ${src})"
}

cache_toolchain() {
    mkdir -p "${tc_dir}"
    local tarball="${tc_dir}/aarch64-toolchain.tar.gz"
    local paths=()
    for p in /usr/aarch64-linux-gnu /usr/lib/gcc-cross/aarch64-linux-gnu \
        /usr/libexec/gcc-cross/aarch64-linux-gnu \
        /usr/bin/aarch64-linux-gnu-gcc-14 /usr/bin/aarch64-linux-gnu-g++-14; do
        [ -e "${p}" ] && paths+=("${p}")
    done
    # Every cross binutil (the ld/ld.bfd symlink pair, as, ar, objdump, …) and
    # the driver symlinks. A glob, because an explicit list silently misses a
    # symlink target and then the restored toolchain cannot link.
    local tool
    for tool in /usr/bin/aarch64-linux-gnu-*; do
        [ -e "${tool}" ] && paths+=("${tool}")
    done
    [ ${#paths[@]} -gt 0 ] || die "no AArch64 cross toolchain found; run scripts/bootstrap.sh first"

    echo "  archiving ${#paths[@]} path(s) into ${tarball} ..."
    # Exclude what a plain `-static` cross build never links, to keep the
    # committed tarball under GitHub's 50 MB recommended per-file size:
    #   * the LTO-only binaries (lto1, lto-dump, lto-wrapper) — the project does
    #     not build with -flto;
    #   * the sanitizer and OpenMP runtimes (libasan/tsan/lsan/ubsan/hwasan,
    #     libgomp, libitm) and the stdc++ extras — cross sanitizer builds are not
    #     part of the suite (ASan/UBSan runs natively), so `bootstrap.sh` is the
    #     path for them.
    tar --exclude='*lto1' --exclude='*lto-dump*' --exclude='*lto-wrapper' \
        --exclude='*libasan*' --exclude='*libtsan*' --exclude='*liblsan*' \
        --exclude='*libubsan*' --exclude='*libhwasan*' --exclude='*libgomp*' \
        --exclude='*libitm*' --exclude='*libstdc++exp*' --exclude='*libstdc++fs*' \
        -czf "${tarball}.tmp" -C / "${paths[@]#/}"
    mv -f "${tarball}.tmp" "${tarball}"
    echo "  cached ${tarball}  ($(du -h "${tarball}" | cut -f1))"
}

# The cross-binutils executables are dynamically linked against host-side
# libopcodes/libbfd libraries whose Debian package names end in `-arm64.so`.
# They are not part of the compiler sysroot, so archiving only /usr/bin and the
# cross GCC directories leaves `aarch64-linux-gnu-as` unusable after restore.
# Keep these small runtime libraries in their own archive rather than rewriting
# the ~50 MB compiler tarball every time a host binutils dependency changes.
cache_binutils_libs() {
    mkdir -p "${tc_dir}"
    local tarball="${tc_dir}/aarch64-binutils-libs.tar.gz"
    local libs=()
    local lib
    for lib in /usr/lib/x86_64-linux-gnu/lib*-arm64.so*; do
        [ -e "${lib}" ] && libs+=("${lib}")
    done
    [ ${#libs[@]} -gt 0 ] || die "no cached host binutils libraries found; install the AArch64 cross toolchain first"

    tar -czf "${tarball}.tmp" -C / "${libs[@]#/}"
    mv -f "${tarball}.tmp" "${tarball}"
    echo "  cached ${tarball}  ($(du -h "${tarball}" | cut -f1))"
}

populate() {
    local with_toolchain=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
        --with-toolchain) with_toolchain=1 ;;
        *) die "unknown argument: $1" ;;
        esac
        shift
    done
    echo "==> populating tool cache at ${cache}"
    cache_qemu
    if [ "${with_toolchain}" -eq 1 ]; then
        cache_toolchain
        cache_binutils_libs
    fi
    echo "==> done"
}

restore() {
    local tarball="${tc_dir}/aarch64-toolchain.tar.gz"
    local libs_tarball="${tc_dir}/aarch64-binutils-libs.tar.gz"
    if [ ! -f "${tarball}" ]; then
        echo "toolcache: no cached toolchain at ${tarball}; run 'populate --with-toolchain' first" >&2
        return 1
    fi
    if [ ! -f "${libs_tarball}" ]; then
        echo "toolcache: no cached AArch64 binutils libraries at ${libs_tarball};" >&2
        echo "           run 'populate --with-toolchain' and commit the generated archive" >&2
        return 1
    fi
    echo "==> restoring AArch64 cross toolchain and binutils runtime libraries (needs root)"
    ${SUDO} tar -xzf "${tarball}" -C /
    ${SUDO} tar -xzf "${libs_tarball}" -C /
    echo "==> restored; verify with: aarch64-linux-gnu-g++ --version && aarch64-linux-gnu-as --version"
}

# Host build tools (cmake/ninja/shellcheck) are not in the base image, so a
# container reset removes them. Small .deb files are committed under
# tools/vendor/host-debs so the build can be restored without a package mirror.
# apt resolves any missing dependency from the network when it can, and reports
# clearly when it cannot.
vendor_dir="${root}/tools/vendor/host-debs"

install_host() {
    if [ ! -d "${vendor_dir}" ]; then
        die "no vendored host tools at ${vendor_dir}"
    fi
    local debs=("${vendor_dir}"/*.deb)
    if [ ! -e "${debs[0]}" ]; then
        die "no .deb files in ${vendor_dir}"
    fi
    echo "==> installing ${#debs[@]} vendored host package(s) (needs root)"
    # --no-download prefers the local files; apt fetches only unmet dependencies.
    if ! ${SUDO} apt-get install -y --no-download "${debs[@]}"; then
        echo "toolcache: offline install failed; retrying with the network for" >&2
        echo "           any missing dependencies ..." >&2
        ${SUDO} apt-get install -y "${debs[@]}" || true
    fi
    # The vendored cmake .deb depends on libarchive13t64, which some base images
    # cannot satisfy, leaving a cmake that fails to load. Detect a non-working
    # cmake and fall back to the self-contained PyPI wheel (no system libs).
    if ! cmake --version >/dev/null 2>&1; then
        echo "toolcache: vendored cmake unusable; falling back to 'pip install cmake'" >&2
        ${SUDO} rm -f /usr/bin/cmake
        python3 -m pip install --quiet cmake
        local wheel_bin
        wheel_bin="$(python3 -c 'import cmake,os;print(os.path.join(os.path.dirname(cmake.__file__),"data","bin"))')"
        ${SUDO} ln -sf "${wheel_bin}/cmake" /usr/bin/cmake
        ${SUDO} ln -sf "${wheel_bin}/ctest" /usr/bin/ctest 2>/dev/null || true
        ${SUDO} ln -sf "${wheel_bin}/cpack" /usr/bin/cpack 2>/dev/null || true
    fi
    echo "==> done; verify with: cmake --version && ninja --version && shellcheck --version"
}

status() {
    echo "cache directory: ${cache}"
    if [ -x "${qemu_cached}" ]; then
        echo "  qemu      : ${qemu_cached} ($(du -h "${qemu_cached}" | cut -f1))"
    else
        echo "  qemu      : not cached"
    fi
    local tarball="${tc_dir}/aarch64-toolchain.tar.gz"
    if [ -f "${tarball}" ]; then
        echo "  toolchain : ${tarball} ($(du -h "${tarball}" | cut -f1))"
    else
        echo "  toolchain : not cached"
    fi
    local libs_tarball="${tc_dir}/aarch64-binutils-libs.tar.gz"
    if [ -f "${libs_tarball}" ]; then
        echo "  binutils  : ${libs_tarball} ($(du -h "${libs_tarball}" | cut -f1))"
    else
        echo "  binutils  : runtime libraries not cached"
    fi
    if [ -d "${vendor_dir}" ]; then
        echo "  host debs : ${vendor_dir} ($(du -sh "${vendor_dir}" 2>/dev/null | cut -f1), committed)"
    else
        echo "  host debs : not vendored"
    fi
    local t
    for t in cmake ninja shellcheck clang-format; do
        printf '  %-10s: %s\n' "${t}" "$(command -v "${t}" 2>/dev/null || echo 'not installed')"
    done
}

print_env() {
    echo "export HYPERION_TOOLCACHE='${cache}'"
    if [ -x "${qemu_cached}" ]; then
        echo "export HYPERION_QEMU='${qemu_cached}'"
        echo "export PATH='${bin_dir}':\"\$PATH\""
    fi
}

command="${1:-status}"
shift || true
case "${command}" in
populate) populate "$@" ;;
restore) restore ;;
install-host) install_host ;;
status) status ;;
env) print_env ;;
*) die "unknown command: ${command} (expected populate|restore|install-host|status|env)" ;;
esac
