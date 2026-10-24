#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# Contract test for install.sh's handling of a test killed by SIGSYS.
#
# `unit.native` calls host kernel interfaces directly. Under some Android/Termux
# setups a seccomp filter or SELinux policy kills such a process with SIGSYS,
# which is the sandbox refusing a system call - not a broken build. install.sh
# asks the user whether to skip that one test instead of failing the install
# outright.
#
# What must hold, and is checked here against a real install.sh run (not a
# reimplementation of its logic):
#
#   1. The question is asked only for that exact situation: the *only* failing
#      test is unit.native and its reason is exactly SIGSYS.
#   2. The default answer is no: Enter, EOF or no terminal at all keeps the test
#      enabled and aborts the install.
#   3. Answering yes re-runs the suite excluding exactly unit.native, and any
#      other failure still aborts the install.
#   4. A skipped test is reported as SKIPPED and is not counted as passing.
#   5. --skip-sigsys-test / HYPERION_SKIP_SIGSYS_TEST=1 skips the question but
#      not the checking.
#   6. A suite in which unit.native passes normally is never skipped, even with
#      that flag given.
#
# usage: install_sigsys_test.sh <path-to-install.sh>
#
# The scenarios build a throwaway CMake project whose only tests are the ones
# named above, so the run is fast and touches nothing outside a temp directory.

set -uo pipefail

INSTALL_SH="${1:-}"
if [ -z "${INSTALL_SH}" ] || [ ! -f "${INSTALL_SH}" ]; then
    printf 'usage: %s <path-to-install.sh>\n' "$0" >&2
    exit 2
fi
INSTALL_SH="$(cd -- "$(dirname -- "${INSTALL_SH}")" && pwd -P)/$(basename -- "${INSTALL_SH}")"

CHECKS=0
FAILURES=0

pass() {
    CHECKS=$((CHECKS + 1))
    printf 'ok   %s\n' "$1"
}

fail() {
    CHECKS=$((CHECKS + 1))
    FAILURES=$((FAILURES + 1))
    printf 'FAIL %s\n' "$1"
}

# check <description> <condition-result>
check() {
    if [ "$2" -eq 0 ]; then
        pass "$1"
    else
        fail "$1"
    fi
}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/hyperion_install_sigsys.XXXXXX")"
cleanup() { rm -rf "${WORK}"; }
trap cleanup EXIT

# The installer reads and creates the syscall registry under
# $XDG_DATA_HOME/$HOME. Point both at the throwaway tree so a run neither
# inspects nor writes the machine's real registry - and so the result does not
# depend on rules somebody happened to have installed.
export XDG_DATA_HOME="${WORK}/xdg-data"
export HOME="${WORK}/home"
mkdir -p "${XDG_DATA_HOME}" "${HOME}"

# A throwaway source tree shaped like this repository: a CMake project with
# `unit.native` plus whatever other tests the scenario needs, and a `scripts/`
# directory the installer can copy from.
make_tree() { # <dir> <unit.native body> <extra test cmake lines or empty>
    local dir="$1" native_body="$2" extra="${3:-}"
    mkdir -p "${dir}/scripts"
    cat > "${dir}/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.20)
# Pin the target before project(): Termux's CMake otherwise decides it is
# building for Android and looks for \$PREFIX/include/android/api-level.h in the
# temporary directory, which does not exist there, and configuration fails for a
# reason that has nothing to do with what this test checks.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_VERSION 29)
project(install_sigsys_fixture NONE)
enable_testing()
add_test(NAME unit.native COMMAND bash -c "${native_body}")
add_test(NAME other.ok COMMAND bash -c "exit 0")
${extra}
EOF
    printf '#!/usr/bin/env bash\nexit 0\n' > "${dir}/scripts/hyperion"
    printf '#!/usr/bin/env bash\nexit 0\n' > "${dir}/scripts/hypy"
    chmod 0755 "${dir}/scripts/hyperion" "${dir}/scripts/hypy"
}

# run_install <tree> <prefix> <answer|-> <extra args...>
#   <answer> = "-" for no controlling terminal / no input at all,
#              otherwise the text typed at the prompt ("" for a bare Enter).
# Prints the installer's combined output; returns the installer's exit status.
run_install() {
    local tree="$1" prefix="$2" answer="$3"
    shift 3
    # --yes suppresses the installer's own "Continue?" confirmation, so the only
    # question in play is the one this test is about. It deliberately does NOT
    # suppress the SIGSYS question: skipping a test is a decision, not a
    # confirmation.
    local args=(--src "${tree}" --prefix "${prefix}" --yes "$@")
    if [ "${answer}" = "-" ]; then
        # stdin is /dev/null and there is no attempt to allocate a tty: this is
        # the `curl | bash` case with nobody at the keyboard.
        setsid bash "${INSTALL_SH}" "${args[@]}" </dev/null 2>&1
        return $?
    fi
    # `script` gives the installer a real controlling terminal, which is the only
    # way to observe the prompt path that a person would actually see.
    if [ -z "${answer}" ]; then
        printf '\n' | script -qec "bash '${INSTALL_SH}' $(printf "'%s' " "${args[@]}")" /dev/null 2>&1
    else
        printf '%s\n' "${answer}" |
            script -qec "bash '${INSTALL_SH}' $(printf "'%s' " "${args[@]}")" /dev/null 2>&1
    fi
}

contains() { case "$1" in *"$2"*) return 0 ;; *) return 1 ;; esac; }

# --- 1. the question is asked, and "yes" completes the install ---------------
say_section() { printf '\n== %s ==\n' "$1"; }

say_section "SIGSYS on unit.native, user answers yes"
TREE1="${WORK}/tree1"
make_tree "${TREE1}" 'kill -s SYS $$'
out="$(run_install "${TREE1}" "${WORK}/p1" yes)"
status=$?
rc=1
[ "${status}" -eq 0 ] && rc=0
check "installer succeeds when the user accepts the skip" "${rc}"
[ "${status}" -eq 0 ] || printf '%s\n' "${out}" | tail -20
contains "${out}" "killed by SIGSYS" && r=0 || r=1
check "it explains that SIGSYS comes from the sandbox" "${r}"
contains "${out}" "Skip 'unit.native' for this installation and continue? [y/N]" && r=0 || r=1
check "it asks before skipping anything" "${r}"
contains "${out}" "not run, not passed" && r=0 || r=1
check "the skipped test is reported as skipped, not as passed" "${r}"
contains "${out}" "all tests passed" && r=1 || r=0
check "it does not claim the whole suite passed" "${r}"
contains "${out}" "every test except unit.native passed" && r=0 || r=1
check "it states that the other tests were re-run and passed" "${r}"
[ -f "${WORK}/p1/share/hyperion/install.conf" ] && r=0 || r=1
check "install.conf is written" "${r}"
if [ -f "${WORK}/p1/share/hyperion/install.conf" ]; then
    grep -q '^HYPERION_SKIPPED_TESTS=unit.native$' "${WORK}/p1/share/hyperion/install.conf" && r=0 || r=1
    check "install.conf records the skipped test" "${r}"
fi
contains "${out}" "unit.native was skipped after being killed by SIGSYS" && r=0 || r=1
check "the final summary repeats the skip" "${r}"

# --- 2. the default is to keep the test --------------------------------------
say_section "SIGSYS on unit.native, user presses Enter (default)"
out="$(run_install "${TREE1}" "${WORK}/p2" "")"
status=$?
[ "${status}" -ne 0 ] && r=0 || r=1
check "a bare Enter keeps the test enabled and aborts the install" "${r}"
contains "${out}" "Keeping the test enabled; nothing was skipped." && r=0 || r=1
check "it says the test was kept" "${r}"
[ -f "${WORK}/p2/share/hyperion/install.conf" ] && r=1 || r=0
check "nothing is installed after the abort" "${r}"

# --- 3. no terminal at all ---------------------------------------------------
say_section "SIGSYS on unit.native, no controlling terminal"
out="$(run_install "${TREE1}" "${WORK}/p3" -)"
status=$?
[ "${status}" -ne 0 ] && r=0 || r=1
check "with nobody to ask, the default (keep the test) applies and install aborts" "${r}"
[ -f "${WORK}/p3/share/hyperion/install.conf" ] && r=1 || r=0
check "nothing is installed in the unattended case" "${r}"

# --- 4. the explicit flag ----------------------------------------------------
say_section "--skip-sigsys-test, SIGSYS on unit.native"
out="$(run_install "${TREE1}" "${WORK}/p4" - --skip-sigsys-test)"
status=$?
[ "${status}" -eq 0 ] && r=0 || r=1
check "--skip-sigsys-test completes the install without a terminal" "${r}"
contains "${out}" "Skip 'unit.native' for this installation and continue?" && r=1 || r=0
check "--skip-sigsys-test does not ask" "${r}"
contains "${out}" "not run, not passed" && r=0 || r=1
check "--skip-sigsys-test still reports the test as skipped" "${r}"
if [ -f "${WORK}/p4/share/hyperion/install.conf" ]; then
    grep -q '^HYPERION_SKIPPED_TESTS=unit.native$' "${WORK}/p4/share/hyperion/install.conf" && r=0 || r=1
    check "--skip-sigsys-test records the skipped test" "${r}"
fi

say_section "HYPERION_SKIP_SIGSYS_TEST=1, SIGSYS on unit.native"
out="$(HYPERION_SKIP_SIGSYS_TEST=1 run_install "${TREE1}" "${WORK}/p5" -)"
status=$?
[ "${status}" -eq 0 ] && r=0 || r=1
check "the environment variable behaves like the flag" "${r}"

# --- 5. an unrelated failure is never swallowed ------------------------------
say_section "SIGSYS on unit.native plus another failing test, user answers yes"
TREE2="${WORK}/tree2"
make_tree "${TREE2}" 'kill -s SYS $$' 'add_test(NAME third.fail COMMAND bash -c "exit 1")'
out="$(run_install "${TREE2}" "${WORK}/p6" yes)"
status=$?
[ "${status}" -ne 0 ] && r=0 || r=1
check "a second failing test aborts the install even after the user says yes" "${r}"
contains "${out}" "Skip 'unit.native' for this installation and continue?" && r=1 || r=0
check "it does not even offer to skip when the failure is not SIGSYS-only" "${r}"
contains "${out}" "refusing to install a broken build" && r=0 || r=1
check "the abort is explicit" "${r}"
[ -f "${WORK}/p6/share/hyperion/install.conf" ] && r=1 || r=0
check "nothing is installed" "${r}"

# --- 6. a failure that merely mentions SIGSYS is not SIGSYS ------------------
say_section "unit.native fails with a message that mentions SIGSYS"
TREE3="${WORK}/tree3"
cat > "${WORK}/mentions_sigsys.sh" <<'SCRIPT'
#!/usr/bin/env bash
# Fails with an ordinary exit status, but its output mentions SIGSYS. The
# installer must go by ctest's own verdict, not by this text.
echo "unexpected SIGSYS in handler"
exit 1
SCRIPT
chmod +x "${WORK}/mentions_sigsys.sh"
make_tree "${TREE3}" "${WORK}/mentions_sigsys.sh"
out="$(run_install "${TREE3}" "${WORK}/p7" yes)"
status=$?
[ "${status}" -ne 0 ] && r=0 || r=1
check "only ctest's own (SIGSYS) verdict counts, not text in the output" "${r}"

# --- 7. control: a healthy suite is never skipped ----------------------------
say_section "control: unit.native passes"
TREE4="${WORK}/tree4"
make_tree "${TREE4}" 'exit 0'
out="$(run_install "${TREE4}" "${WORK}/p8" - --skip-sigsys-test)"
status=$?
[ "${status}" -eq 0 ] && r=0 || r=1
check "a passing suite installs successfully" "${r}"
contains "${out}" "all tests passed" && r=0 || r=1
check "it reports that all tests passed" "${r}"
contains "${out}" "SKIPPED" && r=1 || r=0
check "no test is skipped when nothing failed" "${r}"
if [ -f "${WORK}/p8/share/hyperion/install.conf" ]; then
    grep -q 'HYPERION_SKIPPED_TESTS' "${WORK}/p8/share/hyperion/install.conf" && r=1 || r=0
    check "install.conf carries no skipped-test entry" "${r}"
fi

printf '\n%d checks, %d failure(s)\n' "${CHECKS}" "${FAILURES}"
if [ "${FAILURES}" -ne 0 ]; then
    printf 'install.sh SIGSYS-handling tests: FAILED\n' >&2
    exit 1
fi
printf 'install.sh SIGSYS-handling tests: all checks passed\n'
