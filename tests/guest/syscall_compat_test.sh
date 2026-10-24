#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# End-to-end tests for the syscall compatibility layer, as a guest sees it.
#
# Three things are proven here, and each one would pass vacuously if it were
# omitted:
#
#   1. A real x86-64 binary that uses the newly adapted syscalls runs correctly
#      under HYPERION (compared against the same binary run natively).
#   2. The persistent registry is what enables them: the identical binary run
#      with HYPERION_NO_SYSCALL_REGISTRY=1 must *fail* those calls with -ENOSYS
#      and must not report them as handled.
#   3. A rule written into the registry by the user - a syscall number HYPERION
#      has never heard of - becomes callable, survives being read back, and stops
#      working when the rule is removed.
#
# Usage: syscall_compat_test.sh <hyperion-binary> <fixture-dir>
set -euo pipefail

BIN="${1:?usage: syscall_compat_test.sh <hyperion-binary> <fixture-dir> [emulator]}"
FIXTURE_SRC="${2:?usage: syscall_compat_test.sh <hyperion-binary> <fixture-dir> [emulator]}"
# Same convention as process_test.sh: a third argument (or HYPERION_EMULATOR) is
# a prefix command, used to run an AArch64-built hyperion under qemu-user.
EMULATOR="${3:-${HYPERION_EMULATOR:-}}"
EMULATOR_CMD=()
if [ -n "${EMULATOR}" ]; then
    read -r -a EMULATOR_CMD <<<"${EMULATOR}"
fi

WORK="$(mktemp -d)"

# Keep the run hermetic: without this the CLI creates and reads the *user's*
# real syscall registry under $XDG_DATA_HOME/$HOME, so the test both writes to
# the developer's machine and changes behaviour depending on what is already
# there. tests/harness/test_tempdir.hpp does the same for the C++ tests.
HYPERION_TEST_ROOT="$(mktemp -d)"
export XDG_DATA_HOME="${HYPERION_TEST_ROOT}/xdg-data"
export HOME="${HYPERION_TEST_ROOT}/home"
mkdir -p "$XDG_DATA_HOME" "$HOME"
trap 'rm -rf "$HYPERION_TEST_ROOT" "$WORK"' EXIT

GUEST_TIMEOUT="${HYPERION_GUEST_TIMEOUT:-60s}"
REGISTRY="${WORK}/registry.json"
export HYPERION_SYSCALL_REGISTRY="${REGISTRY}"

FAILED=0
pass() { echo "ok   $*"; }
fail() {
    echo "FAIL $*" >&2
    FAILED=$((FAILED + 1))
}

if [ -n "${HYPERION_GUEST_CC:-}" ]; then
    CC="${HYPERION_GUEST_CC}"
elif [ "$(uname -m)" = "x86_64" ]; then
    CC="cc"
else
    echo "syscall compat tests: no x86-64 fixture compiler (set HYPERION_GUEST_CC)" >&2
    exit 1
fi

for name in compat_syscalls syscall_custom fork_rule_inherit; do
    if ! ${CC} -O2 -static -o "${WORK}/${name}" "${FIXTURE_SRC}/${name}.c" 2>"${WORK}/cc.err"; then
        echo "syscall compat tests: cannot compile ${name}.c with '${CC}'" >&2
        sed 's/^/  /' "${WORK}/cc.err" >&2
        exit 1
    fi
done

# The fixtures must really be static x86-64 ELFs, or this would be testing the
# host's own execution.
for name in compat_syscalls syscall_custom fork_rule_inherit; do
    if ! head -c 20 "${WORK}/${name}" | grep -q 'ELF'; then
        echo "syscall compat tests: ${name} is not an ELF" >&2
        exit 1
    fi
    machine="$(head -c 20 "${WORK}/${name}" | od -An -tu1 -j18 -N2 | awk '{print $1}')"
    if [ "${machine}" != "62" ]; then
        echo "syscall compat tests: ${name} is not x86-64 (e_machine=${machine})" >&2
        exit 1
    fi
done

# Every guest run goes through this helper, so the emulator and the tier apply to
# all of them - including the ones run with the registry disabled.
run() {
    if [ "${#EMULATOR_CMD[@]}" -gt 0 ]; then
        timeout "${GUEST_TIMEOUT}" "${EMULATOR_CMD[@]}" "${BIN}" run --quiet \
            --tier="${HYPERION_TIER:-0}" "$@"
    else
        timeout "${GUEST_TIMEOUT}" "${BIN}" run --quiet --tier="${HYPERION_TIER:-0}" "$@"
    fi
}

# --- 1. native reference -----------------------------------------------------
native_output="$("${WORK}/compat_syscalls" 2>&1)" || true
native="$(printf '%s\n' "${native_output}" | tail -1)"
if [ "${native}" != "failures 0" ]; then
    fail "the fixture does not pass natively (${native}); the test itself is broken"
    printf '%s\n' "${native_output}" | sed 's/^/  native: /' >&2
    exit 1
fi
pass "fixture passes natively"

# --- 1b. terminal raw mode, which needs a real terminal ----------------------
#
# Raw mode is the first thing a terminal application does, and it is a TCSETS
# ioctl. Without a tty the fixture skips that group, so the case is run again
# under `script(1)`, which gives the guest a pseudo-terminal. The native run
# happens first for the same reason as above: the test must not be the thing that
# is wrong about what a terminal should do.
if command -v script >/dev/null 2>&1; then
    tty_native="$(script -qec "${WORK}/compat_syscalls" /dev/null 2>&1 | tr -d '\r')"
    if ! printf '%s\n' "${tty_native}" | grep -q "raw_mode_flags *ok"; then
        fail "the fixture cannot put a pty into raw mode natively; the test is broken"
        printf '%s\n' "${tty_native}" | tail -5 | sed 's/^/  native: /' >&2
    else
        # `script` needs one command string, so the emulator is spliced in.
        tty_out="$(script -qec "${EMULATOR:+${EMULATOR} }${BIN} run --quiet \
            --tier=${HYPERION_TIER:-0} ${WORK}/compat_syscalls" /dev/null 2>&1 | tr -d '\r')"
        if printf '%s\n' "${tty_out}" | grep -q "^failures 0"; then
            pass "terminal attributes and window size work under HYPERION"
        else
            fail "terminal ioctls failed under HYPERION"
            printf '%s\n' "${tty_out}" | grep -E "FAIL|failures" | sed 's/^/  guest: /' >&2
        fi
    fi
else
    echo "skip terminal raw mode (no script(1) on this host)"
fi

# --- 2. with the compatibility registry enabled ------------------------------
if ! out="$(run "${WORK}/compat_syscalls" 2>"${WORK}/err")"; then
    fail "compat_syscalls exited non-zero under HYPERION"
    sed 's/^/  stderr: /' "${WORK}/err" >&2
    printf '%s\n' "${out}" | sed 's/^/  guest: /' >&2
else
    if printf '%s\n' "${out}" | grep -q FAIL; then
        fail "compat_syscalls reported a failing syscall under HYPERION"
        printf '%s\n' "${out}" | sed 's/^/  guest: /' >&2
        sed 's/^/  stderr: /' "${WORK}/err" >&2
    else
        pass "compat syscalls behave correctly under HYPERION"
    fi
fi

# The registry file must have been created with user-only permissions.
if [ -f "${REGISTRY}" ]; then
    mode="$(stat -c '%a' "${REGISTRY}")"
    if [ "${mode}" = "600" ]; then
        pass "registry created with mode 600"
    else
        fail "registry created with mode ${mode}, expected 600"
    fi
else
    fail "the registry was not created at ${REGISTRY}"
fi

# --- 3. no registry file at all still starts and serves the built-in rules ---
#
# `--no-syscall-registry` / HYPERION_NO_SYSCALL_REGISTRY=1 means "do not consult
# the persistent file", not "disable the compatibility layer": the shipped rules
# are compiled into the binary. A missing registry must never stop a guest, so
# this run must behave exactly like case 2, and it must not create a file.
absent="${WORK}/absent/registry.json"
if out="$(HYPERION_NO_SYSCALL_REGISTRY=1 HYPERION_SYSCALL_REGISTRY="${absent}" \
    run "${WORK}/compat_syscalls" 2>"${WORK}/err2")"; then
    if printf '%s\n' "${out}" | grep -q FAIL; then
        fail "the built-in rules alone do not serve the adapted syscalls"
        printf '%s\n' "${out}" | sed 's/^/  guest: /' >&2
        sed 's/^/  stderr: /' "${WORK}/err2" >&2
    else
        pass "built-in rules work with no registry file present"
    fi
else
    fail "the guest failed when the registry file did not exist"
    sed 's/^/  stderr: /' "${WORK}/err2" >&2
fi
if [ -e "${absent}" ]; then
    fail "a registry file was created even though the registry was disabled"
else
    pass "no registry file is created when the registry is disabled"
fi

# --- 4. a user-written rule adds a brand-new syscall -------------------------
cat >"${REGISTRY}" <<'JSON'
{
  "schema_version": 2,
  "guest_arch": "x86_64",
  "comment": "written by tests/guest/syscall_compat_test.sh",
  "rules": [
    {
      "id": "user.test.custom300",
      "name": "custom300",
      "syscall": 300,
      "implementation": "guest_alias",
      "target_syscall": 39,
      "origin": "user",
      "rule_version": 1,
      "enabled": true,
      "notes": "syscall 300 -> getpid(2); there is no such x86-64 syscall"
    }
  ]
}
JSON

out="$(run "${WORK}/syscall_custom" 2>"${WORK}/err3")"
if [ "${out}" = "custom=OK" ]; then
    pass "a user registry rule enables a syscall HYPERION does not implement"
else
    fail "user rule did not take effect (guest printed '${out}')"
    sed 's/^/  stderr: /' "${WORK}/err3" >&2
fi

# --- 4b. a forked child keeps the parent's rules -----------------------------
#
# The child is a separate host process with its own syscall handler. It must use
# the same compiled rules as the parent rather than silently falling back to the
# built-ins.
out="$(run "${WORK}/fork_rule_inherit" 2>"${WORK}/err3b")"
if [ "${out}" = "parent=1 child=1" ]; then
    pass "a forked child inherits the parent's registry rules"
else
    fail "a forked child did not inherit the registry rules (guest printed '${out}')"
    sed 's/^/  stderr: /' "${WORK}/err3b" >&2
fi

# --- 5. removing the rule removes the behaviour ------------------------------
cat >"${REGISTRY}" <<'JSON'
{
  "schema_version": 2,
  "guest_arch": "x86_64",
  "rules": []
}
JSON
out="$(run "${WORK}/syscall_custom" 2>/dev/null)"
if [ "${out}" = "custom=ENOSYS" ]; then
    pass "removing the rule restores -ENOSYS"
else
    fail "removing the rule did not restore -ENOSYS (guest printed '${out}')"
fi

# --- 6. a corrupt registry does not stop HYPERION, and is not overwritten -----
printf '{"schema_version": 2, "rules": [' >"${REGISTRY}"
before="$(cat "${REGISTRY}")"
out="$(run "${WORK}/syscall_custom" 2>"${WORK}/err4")"
if [ "${out}" = "custom=ENOSYS" ]; then
    pass "a corrupt registry falls back to the built-in rules"
else
    fail "a corrupt registry changed guest behaviour (printed '${out}')"
fi
if [ "$(cat "${REGISTRY}")" = "${before}" ]; then
    pass "a corrupt registry was left untouched"
else
    fail "a corrupt registry was overwritten"
fi

echo
if [ "${FAILED}" -ne 0 ]; then
    echo "syscall compat tests: ${FAILED} failure(s)" >&2
    exit 1
fi
echo "syscall compat tests: all checks passed"
