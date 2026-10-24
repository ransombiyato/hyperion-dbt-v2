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
SKIP_SIGSYS="${HYPERION_SKIP_SIGSYS_TEST:-0}"
# Tests this install deliberately did not run (currently only ever `unit.native`,
# after SIGSYS). Recorded in install.conf so the state is visible later instead of
# living only in this script's output.
SKIPPED_TESTS=""

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
  --skip-sigsys-test
                  if `unit.native` is killed by SIGSYS (some Android/Termux
                  setups' seccomp/SELinux policy), skip that one test and
                  continue installing instead of asking. Every other test still
                  has to pass, and the skipped test is reported as skipped, never
                  as passed. Without this flag you are asked first (default: no).
  --yes           do not prompt for confirmation
  --uninstall     remove the installed commands and this install's metadata
  -h, --help      show this help

environment:
  HYPERION_PREFIX  install root
  HYPERION_REF     git ref to build
  HYPERION_SRC     existing source checkout to build from
  HYPERION_SKIP_SIGSYS_TEST=1
                   same as --skip-sigsys-test

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
        --skip-sigsys-test) SKIP_SIGSYS=1; shift ;;
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
# CMake and a C++ compiler are required. Ninja is preferred but not required:
# CMake's Unix Makefiles generator produces the same build, and a minimal Termux
# install may not have ninja (it is a separate package from cmake).
need_tool cmake || missing="${missing} cmake"
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

if need_tool ninja; then
    GENERATOR="Ninja"
else
    GENERATOR="Unix Makefiles"
    warn "ninja not found; using CMake's Unix Makefiles generator instead"
    warn "(install it with 'pkg install ninja' on Termux for faster builds)"
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

# --- the test suite ---------------------------------------------------------
#
# Do not install a command that points at a build that fails its own tests.
#
# The one exception is environmental, and it is the user's decision, not this
# script's: `unit.native` calls host kernel interfaces directly, and some
# Android/Termux setups (an SELinux policy or a seccomp filter on the app
# process) answer one of those calls by killing the process with SIGSYS. That is
# the sandbox refusing a syscall, not a defect in HYPERION, and no amount of
# retrying changes it.
#
# So: when - and only when - the single failing test is `unit.native` and its
# reported failure reason is exactly SIGSYS, explain the situation and ask. The
# default answer is no (keep the test, abort the install), because turning off a
# check silently is how real failures get missed later. If the user says yes, the
# suite is re-run excluding exactly that one test; every other test still has to
# pass or the install still aborts, and the skipped test is reported as SKIPPED -
# never as passed, and never counted as coverage that happened.

# Yes/no question, default "no". Reads from the controlling terminal, not stdin:
# `curl ... | bash` feeds the *script* through stdin, so a plain `read` would
# consume the rest of the installer instead of the user's answer. With no
# terminal at all there is nobody to ask, so the default (no) applies.
ask_yes_no() {
    local prompt="$1" reply=""
    if [ -r /dev/tty ]; then
        printf '%s [y/N] ' "${prompt}" >/dev/tty
        IFS= read -r reply </dev/tty || reply=""
    elif [ -t 0 ]; then
        printf '%s [y/N] ' "${prompt}"
        IFS= read -r reply || reply=""
    else
        return 1
    fi
    case "${reply}" in
        [yY] | [yY][eE][sS]) return 0 ;;
        *) return 1 ;;
    esac
}

# Names of the tests ctest reported as failed, as ctest lists them at the end of
# its run: "<n> - <name> (<reason>)".
failed_test_list() {
    sed -n '/The following tests FAILED:/,$p' \
        | sed -n 's/^[[:space:]]*[0-9][0-9]* - \(.*\)$/\1/p'
}

run_test_suite() {
    local output status=0 failed
    # stdin is /dev/null on purpose: a test that reads the terminal would
    # otherwise swallow the answer to the question asked below.
    output="$(ctest --test-dir "${BUILD_DIR}" --output-on-failure </dev/null 2>&1)" || status=$?
    if [ "${status}" -eq 0 ]; then
        info "all tests passed"
        return 0
    fi

    # A real failure: show ctest's own output rather than a summary of it.
    printf '%s\n' "${output}"
    failed="$(printf '%s\n' "${output}" | failed_test_list)"

    # Exactly one failure, and it is unit.native killed by SIGSYS. Anything else
    # - a second failing test, a different reason, a timeout, a crash - is a
    # genuine failure and aborts right here. This comparison is deliberately
    # exact so a broken build cannot slip through on a substring match.
    if [ "${failed}" != "unit.native (SIGSYS)" ]; then
        die "tests failed; refusing to install a broken build"
    fi

    if [ "${SKIP_SIGSYS}" -eq 1 ]; then
        info "--skip-sigsys-test: skipping the SIGSYS-killed test (not asking)"
    else
        say ""
        say "  'unit.native' was killed by SIGSYS before it could finish."
        if [ "${IS_TERMUX}" -eq 1 ]; then
            say "  This is a Termux installation, and Termux's own seccomp/SELinux"
            say "  policy is the likeliest source of that signal."
        fi
        say ""
        say "  SIGSYS is normally raised by a seccomp filter or an SELinux policy"
        say "  terminating a process that made a disallowed system call. On some"
        say "  Android/Termux setups that is what happens to this test binary: the"
        say "  device's sandbox refuses a call the test makes, and the process is"
        say "  killed. It is a property of this environment, not a bug in the build."
        say ""
        say "  If you skip it, this one test is NOT run and NOT reported as"
        say "  passing: it stays unverified on this device. Every other test still"
        say "  has to pass, or this installation will abort. You can re-run it once"
        say "  the sandbox allows the call:"
        say "    hyperion test            (or: ctest -R '^unit\\.native\$')"
        say ""
        if ! ask_yes_no "  Skip 'unit.native' for this installation and continue?"; then
            say ""
            say "  Keeping the test enabled; nothing was skipped."
            die "tests failed; refusing to install a broken build"
        fi
    fi
    SKIPPED_TESTS="unit.native"

    # Re-run everything except that one test. Any other failure - including a
    # flaky one that only shows up on the second run - still aborts the install.
    say "Running the remaining tests (excluding unit.native)"
    if ! ctest --test-dir "${BUILD_DIR}" --output-on-failure -E '^unit\.native$' </dev/null; then
        die "tests failed with unit.native excluded; refusing to install a broken build"
    fi
    info "every test except unit.native passed"
    info "unit.native: SKIPPED after SIGSYS - not run, not passed, still unverified"
}

# --- build and test ---------------------------------------------------------

BUILD_DIR="${SRC_DIR}/build/install"

if [ "$DO_BUILD" -eq 1 ]; then
    say "Configuring (Release)"
    cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" -G "${GENERATOR}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DHYPERION_WARNINGS_AS_ERRORS=ON >/dev/null \
        || die "configure failed (see ${BUILD_DIR}/CMakeFiles/CMakeConfigureLog.yaml)"

    say "Building"
    cmake --build "${BUILD_DIR}" >/dev/null || die "build failed"

    say "Running the test suite"
    run_test_suite

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
    # Present only when a test was deliberately skipped. Its presence means
    # exactly that: "not run", never "passed".
    if [ -n "${SKIPPED_TESTS}" ]; then
        printf 'HYPERION_SKIPPED_TESTS=%s\n' "${SKIPPED_TESTS}"
    fi
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
    # The check output goes to a real temporary file. It used to be written to
    # "${TMP_DIR:-/tmp}", and TMP_DIR is never set: on Termux, which has no /tmp,
    # the redirection failed, the check was reported as unvalidated, and the
    # "details" could not be shown either. mktemp honours $TMPDIR, which is what
    # Termux provides.
    REGISTRY_CHECK="$(mktemp "${TMPDIR:-/tmp}/hyperion-registry-check.XXXXXX" 2>/dev/null \
                      || mktemp "${PREFIX}/tmp/hyperion-registry-check.XXXXXX" 2>/dev/null \
                      || echo "")"
    if [ -z "${REGISTRY_CHECK}" ]; then
        warn "no writable temporary directory for the registry check output"
        REGISTRY_CHECK="/dev/null"
    fi
    # An empty HYPERION_NO_SYSCALL_REGISTRY= would disable the very check being
    # run, so it is unset for this one command rather than cleared.
    if env -u HYPERION_NO_SYSCALL_REGISTRY "${BIN_DIR}/hyperion" syscall-registry \
        --validate --registry="${REGISTRY_PATH}" >"${REGISTRY_CHECK}" 2>&1; then
        info "registry validates"
    else
        warn "the registry did not validate; HYPERION will fall back to its built-in"
        warn "rules at run time rather than refusing to start. Details:"
        sed 's/^/  /' "${REGISTRY_CHECK}" >&2 || true
    fi
    rm -f "${REGISTRY_CHECK}" 2>/dev/null || true
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
if [ -n "${SKIPPED_TESTS}" ]; then
    say ""
    say "Note: the test ${SKIPPED_TESTS} was skipped after being killed by SIGSYS."
    say "      It did not pass - it was not run. Run it with: hyperion test"
fi
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
