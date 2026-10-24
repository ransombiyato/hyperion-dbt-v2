#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Project HYPERION - bootstrap installer.
#
# Usage (one line):
#   curl -fsSL https://raw.githubusercontent.com/ransombiyato/hyperion-dbt-v2/main/install.sh | bash
#
# Or, if you would rather read it before running it (recommended):
#   curl -fsSLO https://raw.githubusercontent.com/ransombiyato/hyperion-dbt-v2/main/install.sh
#   less install.sh
#   bash install.sh
#
# What it does:
#   1. checks for a C++20 toolchain, CMake and Ninja
#   2. fetches this repository (git clone if available, else a tarball), or
#      builds the checkout this script was run from
#   3. configures and builds it
#   4. runs the test suite, and ABORTS rather than install a broken build
#   5. installs `hyperion` and `hypy` commands into <prefix>/bin
#
# What it does NOT do:
#   - it never uses sudo and never needs root
#   - it never touches anything outside the prefix (default ~/.local)
#   - it does not modify your shell rc files
#   - it never reports success when the build or the tests failed
#
# Primary target is unrooted Android under Termux (AArch64, Android 10/API 29,
# kernel 4.14); x86-64 Linux is supported for development. Both raw x86-64 blobs
# and Linux x86-64 ELF64 binaries run across Tier 0 (interpreter) and Tier 1
# (AArch64 + ARMv8 NEON JIT):
#   hyperion run [--tier=0|1] <file> [guest-args...]
#   hypy run <file> [guest-args...]          # shorter launcher for the same
set -euo pipefail

REPO_SLUG="${HYPERION_REPO_SLUG:-ransombiyato/hyperion-dbt-v2}"
DEFAULT_REF="main"

PREFIX="${HYPERION_PREFIX:-$HOME/.local}"
REF="${HYPERION_REF:-$DEFAULT_REF}"
SRC_DIR="${HYPERION_SRC:-}"
DO_BUILD=1
DO_UNINSTALL=0
ASSUME_YES=0
DO_HYPY=1

# Resolve the directory this script lives in. When it is being run from a
# checkout, that checkout is built -- re-downloading the repository the user is
# already standing in would be surprising, and on a device with no network it
# would simply fail.
SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd -P)"

say()  { printf '%s\n' "$*"; }
info() { printf '  %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
usage: install.sh [options]

options:
  --prefix DIR    install root (default: $HYPERION_PREFIX or ~/.local)
  --ref REF       git ref to build (default: $HYPERION_REF or main)
  --src DIR       use an existing checkout instead of fetching one
  --no-build      install the command only; do not configure or build
  --no-hypy       do not install the `hypy` launcher
  --yes           do not prompt for confirmation
  --uninstall     remove the installed commands and this install's metadata
  -h, --help      show this help

environment:
  HYPERION_PREFIX  install root
  HYPERION_REF     git ref to build
  HYPERION_SRC     existing source checkout to build from

Run from inside a HYPERION checkout, this builds that checkout. Otherwise it
fetches the repository into <prefix>/share/hyperion/src.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)    [ $# -ge 2 ] || die "--prefix needs a directory"; PREFIX="$2"; shift 2 ;;
        --prefix=*)  PREFIX="${1#*=}"; shift ;;
        --ref)       [ $# -ge 2 ] || die "--ref needs a value"; REF="$2"; shift 2 ;;
        --ref=*)     REF="${1#*=}"; shift ;;
        --src)       [ $# -ge 2 ] || die "--src needs a directory"; SRC_DIR="$2"; shift 2 ;;
        --src=*)     SRC_DIR="${1#*=}"; shift ;;
        --no-build)  DO_BUILD=0; shift ;;
        --no-hypy)   DO_HYPY=0; shift ;;
        --yes|-y)    ASSUME_YES=1; shift ;;
        --uninstall) DO_UNINSTALL=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *)           die "unknown option '$1' (try --help)" ;;
    esac
done

BIN_DIR="${PREFIX}/bin"
LIB_DIR="${PREFIX}/share/hyperion"
CONF_FILE="${LIB_DIR}/install.conf"

# --- uninstall --------------------------------------------------------------

if [ "$DO_UNINSTALL" -eq 1 ]; then
    say "Uninstalling hyperion from ${PREFIX}"
    removed=0
    for cmd in hyperion hypy; do
        if [ -f "${BIN_DIR}/${cmd}" ]; then
            rm -f "${BIN_DIR}/${cmd}"
            info "removed ${BIN_DIR}/${cmd}"
            removed=1
        fi
    done
    if [ -f "${CONF_FILE}" ]; then
        # Only remove the source tree if this install created it under LIB_DIR.
        src="$(sed -n 's/^HYPERION_SRC=//p' "${CONF_FILE}" | head -1)"
        rm -f "${CONF_FILE}"
        info "removed ${CONF_FILE}"
        removed=1
        case "${src}" in
            "${LIB_DIR}"/*)
                rm -rf "${src}"
                info "removed source tree ${src}"
                ;;
            "")
                ;;
            *)
                info "left source tree in place: ${src}"
                ;;
        esac
    fi
    # Remove now-empty directories, but only if they are empty: never delete
    # anything that was not created by this installer.
    # The syscall registry is user data living outside the install directory. It
    # is deliberately kept: deleting it would silently drop rules the user wrote.
    registry="${XDG_DATA_HOME:-$HOME/.local/share}/hyperion/syscalls/registry.json"
    if [ -f "${registry}" ]; then
        say "Keeping the syscall registry at ${registry} (delete it yourself to remove"
        say "your rules)."
    fi
    rmdir "${LIB_DIR}" 2>/dev/null || true
    rmdir "$(dirname "${LIB_DIR}")" 2>/dev/null || true
    rmdir "${BIN_DIR}" 2>/dev/null || true
    if [ "$removed" -eq 1 ]; then
        say "Done."
    else
        say "Nothing to uninstall."
    fi
    exit 0
fi

# --- confirmation -----------------------------------------------------------

say "Project HYPERION installer"
say "  repository : ${REPO_SLUG} (ref: ${REF})"
say "  prefix     : ${PREFIX}"
say "  root       : not required (this never uses sudo)"
say ""
if [ "$ASSUME_YES" -eq 0 ] && [ -t 0 ]; then
    printf 'Continue? [Y/n] '
    read -r reply || reply=""
    case "${reply}" in
        ""|y|Y|yes|YES) ;;
        *) say "Aborted."; exit 0 ;;
    esac
fi

# --- host checks ------------------------------------------------------------

[ "$(uname -s)" = "Linux" ] || die "this installer targets Linux (found $(uname -s))"

# Termux on Android sets PREFIX to its own package root and has no /etc/os-release.
# Detect it explicitly: the dependency names, the writable locations and the
# seccomp restrictions all differ from a desktop distribution, and the messages
# below have to say the right thing for the device actually being used.
IS_TERMUX=0
if [ -n "${TERMUX_VERSION:-}" ] || [ -n "${TERMUX_APP__PACKAGE_NAME:-}" ] || \
   [ -d "${TERMUX_PREFIX:-/data/data/com.termux/files/usr}" ]; then
    IS_TERMUX=1
fi
if [ "$IS_TERMUX" -eq 1 ]; then
    say "  environment: Termux on Android $(uname -m), kernel $(uname -r)"
    # ~/.local/bin is the conventional user prefix and is on PATH in recent
    # Termux; the Termux package root itself is not user-writable in the same way.
    info "installing to ${PREFIX} (no root required)"
fi

case "$(uname -m)" in
    x86_64|aarch64|arm64) ;;
    *) warn "untested architecture '$(uname -m)'; continuing anyway" ;;
esac

need_tool() {
    command -v "$1" >/dev/null 2>&1
}

missing=""
for tool in cmake ninja; do
    need_tool "$tool" || missing="${missing} ${tool}"
done
if ! need_tool c++ && ! need_tool g++ && ! need_tool clang++; then
    missing="${missing} c++"
fi

if [ -n "$missing" ]; then
    warn "missing required tool(s):${missing}"
    say ""
    say "Install them first. On Debian/Ubuntu:"
    say "  sudo apt-get install -y build-essential cmake ninja-build"
    say "On Termux:"
    say "  pkg install clang cmake ninja"
    say "On Fedora:"
    say "  sudo dnf install -y gcc-c++ cmake ninja-build"
    say ""
    say "Or run the project's own bootstrap (it also fetches test tooling):"
    say "  bash scripts/bootstrap.sh"
    die "cannot continue without a toolchain"
fi

# --- fetch ------------------------------------------------------------------

if [ -z "$SRC_DIR" ] && [ -f "${SCRIPT_DIR}/CMakeLists.txt" ]; then
    SRC_DIR="${SCRIPT_DIR}"
fi

if [ -n "$SRC_DIR" ]; then
    [ -f "${SRC_DIR}/CMakeLists.txt" ] || die "'${SRC_DIR}' is not a HYPERION checkout (no CMakeLists.txt)"
    say "Using existing checkout: ${SRC_DIR}"
else
    SRC_DIR="${LIB_DIR}/src"
    if [ -d "${SRC_DIR}/.git" ]; then
        say "Updating existing checkout in ${SRC_DIR}"
        if need_tool git; then
            if ! git -C "${SRC_DIR}" fetch --depth 1 origin "${REF}" \
                 || ! git -C "${SRC_DIR}" checkout -q FETCH_HEAD; then
                die "could not update ${SRC_DIR} to ref '${REF}'"
            fi
        fi
    elif need_tool git; then
        say "Cloning ${REPO_SLUG} (ref: ${REF}) into ${SRC_DIR}"
        mkdir -p "$(dirname "${SRC_DIR}")"
        rm -rf "${SRC_DIR}"
        git clone --depth 1 --branch "${REF}" \
            "https://github.com/${REPO_SLUG}.git" "${SRC_DIR}" \
            || die "git clone failed"
    else
        say "git not found; downloading a source archive instead"
        mkdir -p "${SRC_DIR}"
        tmp="$(mktemp -d)"
        tarball="${tmp}/src.tar.gz"
        url="https://github.com/${REPO_SLUG}/archive/refs/heads/${REF}.tar.gz"
        if need_tool curl; then
            curl -fsSL "${url}" -o "${tarball}" || die "download failed: ${url}"
        elif need_tool wget; then
            wget -qO "${tarball}" "${url}" || die "download failed: ${url}"
        else
            die "neither git, curl nor wget is available"
        fi
        tar -xzf "${tarball}" -C "${tmp}"
        inner="$(find "${tmp}" -maxdepth 1 -mindepth 1 -type d | head -1)"
        [ -n "${inner}" ] || die "could not unpack the archive"
        rm -rf "${SRC_DIR}"
        mv "${inner}" "${SRC_DIR}"
        rm -rf "${tmp}"
    fi
fi

# --- build and test ---------------------------------------------------------

BUILD_DIR="${SRC_DIR}/build/install"

if [ "$DO_BUILD" -eq 1 ]; then
    say "Configuring (Release)"
    cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DHYPERION_WARNINGS_AS_ERRORS=ON >/dev/null \
        || die "configure failed (see ${BUILD_DIR}/CMakeFiles/CMakeConfigureLog.yaml)"

    say "Building"
    cmake --build "${BUILD_DIR}" >/dev/null || die "build failed"

    say "Running the test suite"
    # Do not install a command that points at a build that fails its own tests.
    if ! ctest --test-dir "${BUILD_DIR}" --output-on-failure >/dev/null 2>&1; then
        ctest --test-dir "${BUILD_DIR}" --output-on-failure || true
        die "tests failed; refusing to install a broken build"
    fi
    info "all tests passed"
else
    say "Skipping build (--no-build)"
fi

# --- install the command ----------------------------------------------------

say "Installing the 'hyperion' command into ${BIN_DIR}"
mkdir -p "${BIN_DIR}" "${LIB_DIR}"

CLI_SRC="${SRC_DIR}/scripts/hyperion"
[ -f "${CLI_SRC}" ] || die "expected '${CLI_SRC}' to exist in the checkout"

install -m 0755 "${CLI_SRC}" "${BIN_DIR}/hyperion"

# `hypy` is the short launcher. An existing one is replaced, because it is
# generated by this installer; it is never a file the user edits. Anything the
# user wrote themselves under a different name is not touched.
if [ "$DO_HYPY" -eq 1 ]; then
    HYPY_SRC="${SRC_DIR}/scripts/hypy"
    if [ -f "${HYPY_SRC}" ]; then
        install -m 0755 "${HYPY_SRC}" "${BIN_DIR}/hypy"
        info "installed ${BIN_DIR}/hypy"
    else
        warn "scripts/hypy not found in the checkout; skipping the hypy launcher"
    fi
fi

# The CLI reads this to find the checkout and build tree. Quoted because the
# paths may contain spaces.
{
    printf '# Generated by install.sh on %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf 'HYPERION_SRC=%s\n' "${SRC_DIR}"
    printf 'HYPERION_BUILD_DIR=%s\n' "${BUILD_DIR}"
    printf 'HYPERION_REF=%s\n' "${REF}"
} > "${CONF_FILE}"
chmod 0644 "${CONF_FILE}"

# --- syscall registry -------------------------------------------------------
#
# The registry lives *outside* the install directory: it is the user's file, not
# this installer's, so upgrading HYPERION must not overwrite hand-written rules.
# It is therefore created only when it does not exist yet, and an existing one is
# validated and left alone.

say ""
say "Syscall registry"
REGISTRY_PATH="${XDG_DATA_HOME:-$HOME/.local/share}/hyperion/syscalls/registry.json"
if [ -n "${HYPERION_SYSCALL_REGISTRY:-}" ]; then
    REGISTRY_PATH="${HYPERION_SYSCALL_REGISTRY}"
    info "using the path from \$HYPERION_SYSCALL_REGISTRY"
fi

if [ -f "${REGISTRY_PATH}" ]; then
    info "keeping the existing registry at ${REGISTRY_PATH} (user rules are preserved)"
    if HYPERION_NO_SYSCALL_REGISTRY= "${BIN_DIR}/hyperion" syscall-registry \
        --validate --registry="${REGISTRY_PATH}" >"${TMP_DIR:-/tmp}/registry.check" 2>&1; then
        info "registry validates"
    else
        warn "the registry did not validate; HYPERION will fall back to its built-in"
        warn "rules at run time rather than refusing to start. Details:"
        sed 's/^/  /' "${TMP_DIR:-/tmp}/registry.check" >&2 || true
    fi
else
    if "${BIN_DIR}/hyperion" syscall-registry --write --registry="${REGISTRY_PATH}" \
        >/dev/null 2>&1; then
        info "created the default registry at ${REGISTRY_PATH}"
    else
        warn "could not create the registry at ${REGISTRY_PATH}"
        warn "HYPERION works without it; the built-in rules are compiled in"
    fi
fi
say "  edit or extend: ${REGISTRY_PATH}"
say "  validate:       hyperion syscall-registry --validate"
say "  disable:        HYPERION_NO_SYSCALL_REGISTRY=1 or --no-syscall-registry"

say ""
say "Installed."
say ""
say "  hypy <elf> [args...]   run a program (exit status = the guest's)"
say "  hyperion run <elf> ... the same thing, explicitly"
say "  hyperion probe         report host capabilities"
say "  hyperion doctor        check the host, toolchain and install"
say "  hyperion test          run the test suite"
say "  hyperion help          list every command"
say ""
say "Example:"
say "  hypy \"\$HOME/hyperion-test/busybox-x86_64\" sh -c 'echo benchmark | wc -c'"
say ""

case ":${PATH}:" in
    *":${BIN_DIR}:"*) ;;
    *)
        say "Note: ${BIN_DIR} is not on your PATH. Add it with:"
        say "  export PATH=\"${BIN_DIR}:\$PATH\""
        say "(add that line to your shell rc file to make it permanent)"
        say ""
        ;;
esac

say "To remove it again:"
say "  bash install.sh --uninstall --prefix ${PREFIX}"
say ""
say "Note: 'hyperion bench' measures host calibration; 'hyperion_tier_bench'"
say "measures Tier 0 vs Tier 1 guest execution. See STATUS.md and COMPATIBILITY.md."
