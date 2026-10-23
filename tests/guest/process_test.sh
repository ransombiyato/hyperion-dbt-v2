#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# Guest process regression tests.
#
# These run real x86-64 Linux programs under HYPERION and compare against the
# native result. They exist because a translator can report "status: ok" for a
# guest that quietly failed: `sh: can't fork: Function not implemented` is a
# guest failure even when the outer run reports success. So every case here
# checks the guest's *output* and its *exit status*, never the translator's.
#
# Fixtures are compiled from tests/guest/fixtures/*.c by this script, using the
# same mechanism the rest of the project uses for guest binaries
# (`gcc -O2 -static`, see tests/cli/cli_smoke.sh and
# scripts/run_real_benchmarks.sh). They are never substituted with host-arch
# binaries: an AArch64 binary is not an x86-64 guest test.
#
# Usage: process_test.sh <hyperion-binary> <fixture-dir> [emulator]
#
# Environment:
#   HYPERION_GUEST_CC   cross compiler producing static x86-64 ELFs
#                       (default: cc, only valid on an x86-64 host)
#   HYPERION_BUSYBOX    static x86-64 BusyBox for the shell-pipeline test;
#                       when unset, that one test is reported as SKIPPED
set -euo pipefail

BIN="${1:?usage: process_test.sh <hyperion-binary> <fixture-dir> [emulator]}"
FIXTURE_SRC="${2:?usage: process_test.sh <hyperion-binary> <fixture-dir> [emulator]}"
EMULATOR="${3:-${HYPERION_EMULATOR:-}}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# shellcheck disable=SC2086
run_hyperion() { $EMULATOR "$BIN" "$@"; }

FAILED=0
SKIPPED=0

# run_case <label> <guest-program> <expected-stdout> [guest-args...]
# Asserts the guest's stdout matches exactly and that its exit status is 0.
run_case() {
    local label="$1" prog="$2" expected="$3"
    shift 3
    local out rc
    out="$($EMULATOR "$BIN" run --quiet --tier="${HYPERION_TIER:-0}" "$prog" "$@" 2>"$WORK/err")" && rc=0 || rc=$?
    if [ "$out" != "$expected" ]; then
        echo "FAIL ${label}: stdout mismatch" >&2
        echo "  expected: $(printf '%s' "$expected" | tr '\n' '|')" >&2
        echo "  actual:   $(printf '%s' "$out" | tr '\n' '|')" >&2
        # An empty result with a zero status means the guest produced nothing at
        # all, which is a different failure from a wrong answer and needs the
        # status and stderr to diagnose. Report both rather than just the diff.
        echo "  guest exit status: ${rc}" >&2
        sed 's/^/  stderr: /' "$WORK/err" >&2
        FAILED=$((FAILED + 1))
        return
    fi
    if [ "$rc" -ne 0 ]; then
        echo "FAIL ${label}: exit status ${rc} (expected 0)" >&2
        FAILED=$((FAILED + 1))
        return
    fi
    echo "ok   ${label}"
}

# --- compile the fixtures ----------------------------------------------------

if [ -n "${HYPERION_GUEST_CC:-}" ]; then
    CC="${HYPERION_GUEST_CC}"
elif [ "$(uname -m)" = "x86_64" ]; then
    CC="cc"
else
    echo "process tests: no x86-64 fixture compiler (set HYPERION_GUEST_CC)" >&2
    exit 1
fi

for src in "${FIXTURE_SRC}"/*.c; do
    name="$(basename "${src}" .c)"
    if ! ${CC} -O2 -static -o "${WORK}/${name}" "${src}" 2>"${WORK}/cc.err"; then
        echo "process tests: cannot compile ${name}.c with '${CC}'" >&2
        sed 's/^/  /' "${WORK}/cc.err" >&2
        exit 1
    fi
done

# Every fixture must really be a static x86-64 ELF, or the test would be
# measuring the host's own execution rather than the translator.
for f in "${WORK}"/*; do
    [ -x "$f" ] || continue
    if ! head -c 20 "$f" | grep -q 'ELF'; then
        echo "process tests: $(basename "$f") is not an ELF" >&2
        exit 1
    fi
    machine="$(head -c 20 "$f" | od -An -tu1 -j18 -N2 | tr -s ' ')"
    # e_machine lives at offset 18; 62 0 (little-endian) is EM_X86_64.
    if [ "$(echo "$machine" | awk '{print $1}')" != "62" ]; then
        echo "process tests: $(basename "$f") is not x86-64 (e_machine=${machine})" >&2
        exit 1
    fi
done

# --- the cases ---------------------------------------------------------------

run_case "fork + waitpid exit status" "${WORK}/fork_exit_status" \
    "$(printf 'child running\nPASS fork status=7 pid_match=1')"

run_case "execve of a second program" "${WORK}/execve_child" \
    "$(printf 'target argv1=hello\nPASS execve status=3')"

run_case "file descriptor inheritance" "${WORK}/fd_inherit" \
    "PASS fd_inherit bytes=10"

run_case "pipe + fork IPC" "${WORK}/pipe_ipc" \
    "PASS pipe got=5"

run_case "vfork" "${WORK}/vfork_basic" \
    "PASS vfork status=9"

# bad_args prints one line per case; only the final PASS line is pinned, because
# the intermediate lines are diagnostics whose exact wording is not contract.
out="$($EMULATOR "$BIN" run --quiet --tier="${HYPERION_TIER:-0}" "${WORK}/bad_args" 2>"$WORK/err")" \
    && rc=0 || rc=$?
if ! printf '%s\n' "$out" | tail -1 | grep -q '^PASS bad_args$'; then
    echo "FAIL unsupported clone flags / invalid arguments" >&2
    printf '%s\n' "$out" | sed 's/^/  /' >&2
    FAILED=$((FAILED + 1))
elif [ "$rc" -ne 0 ]; then
    echo "FAIL bad_args exit status ${rc}" >&2
    FAILED=$((FAILED + 1))
else
    echo "ok   unsupported clone flags / invalid arguments"
fi

# raw_clone_flags pins the translator's *documented divergence* from native
# Linux: it refuses CLONE_VM and friends with ENOSYS rather than silently
# forking a guest that asked for a shared address space. This one is expected to
# FAIL on native Linux, so it is deliberately not compared against a native run.
out="$($EMULATOR "$BIN" run --quiet --tier="${HYPERION_TIER:-0}" "${WORK}/raw_clone_flags" 2>"$WORK/err")" \
    && rc=0 || rc=$?
if ! printf '%s\n' "$out" | tail -1 | grep -q '^PASS raw_clone_flags$'; then
    echo "FAIL raw clone flags must be refused with ENOSYS" >&2
    printf '%s\n' "$out" | sed 's/^/  /' >&2
    FAILED=$((FAILED + 1))
elif [ "$rc" -ne 0 ]; then
    echo "FAIL raw_clone_flags exit status ${rc}" >&2
    FAILED=$((FAILED + 1))
else
    echo "ok   raw clone: unsupported flags refused with ENOSYS"
fi

# --- shell pipeline through a real static x86-64 BusyBox ---------------------

BUSYBOX="${HYPERION_BUSYBOX:-}"
if [ -n "$BUSYBOX" ] && [ -x "$BUSYBOX" ]; then
    run_case "busybox sh pipeline (printf | sort -n)" "$BUSYBOX" \
        "$(printf '1\n2\n5\n9')" sh -c 'printf "9\n2\n5\n1\n" | sort -n'
    run_case "busybox sh pipeline (echo | wc -c)" "$BUSYBOX" \
        "10" sh -c 'echo benchmark | wc -c'
else
    echo "skip busybox shell pipeline (set HYPERION_BUSYBOX to a static x86-64 busybox)"
    SKIPPED=$((SKIPPED + 2))
fi

if [ "$FAILED" -ne 0 ]; then
    echo "process tests FAILED: ${FAILED} case(s), ${SKIPPED} skipped" >&2
    exit 1
fi
echo "process tests ok (${SKIPPED} skipped)"
