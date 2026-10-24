#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# `hyperion` wrapper test.
#
# scripts/hyperion is the user-facing command installed by install.sh. It is a
# thin front end over the built binary, so it can silently drift out of date
# (it once claimed the translator did not exist and refused `run`). This test
# pins the two properties that matter:
#
#   1. `status` does not deny features that exist (the translator, code
#      generation, instruction decoding).
#   2. `run` reaches the real binary and executes a blob, instead of printing
#      NOT IMPLEMENTED.
#
# Usage: wrapper_test.sh <path-to-hyperion-binary>
#   HYPERION_SRC / HYPERION_BUILD_DIR  point the wrapper at the test build.
set -euo pipefail

BIN="${1:?usage: wrapper_test.sh <hyperion-binary>}"
WRAPPER="$(dirname "${BASH_SOURCE[0]}")/../../scripts/hyperion"
WRAPPER="$(cd "$(dirname "${WRAPPER}")" && pwd)/$(basename "${WRAPPER}")"

fail() {
    echo "wrapper test FAILED: $*" >&2
    exit 1
}

# `status` must be honest: it may not claim the translator is absent.
status="$(bash "${WRAPPER}" status)"
if grep -qi 'no way to run an x86-64 program' <<<"$status"; then
    fail "'status' still claims no x86-64 program can run"
fi
if grep -qiE '^[[:space:]]*- x86-64 instruction decoding$' <<<"$status"; then
    fail "'status' still lists instruction decoding as NOT IMPLEMENTED"
fi
if ! grep -q 'run <file>' <<<"$(bash "${WRAPPER}" help)"; then
    fail "'help' does not advertise the run command"
fi

# `help` must advertise the packaging command too: it is how a user gets a
# binary without a toolchain (see README "Getting a binary without building").
if ! grep -q '^  package ' <<<"$(bash "${WRAPPER}" help)"; then
    fail "'help' does not advertise the package command"
fi

# `info` is implemented now (the ELF64 loader exists); it must delegate rather
# than claim NOT IMPLEMENTED, and must describe a real ELF image.
if ! grep -q 'info <elf>' <<<"$(bash "${WRAPPER}" help)"; then
    fail "'help' does not advertise the info command"
fi
FIXTURE="$(dirname "${BASH_SOURCE[0]}")/../fixtures/tiny-static.elf"
info_out="$(bash "${WRAPPER}" info "$FIXTURE" 2>&1)" || fail "'info' exited non-zero"
if grep -q 'NOT IMPLEMENTED' <<<"$info_out"; then
    fail "'info' printed NOT IMPLEMENTED instead of delegating"
fi
grep -q '^type:.*static executable' <<<"$info_out" || fail "'info' did not describe the ELF"

# `run` must delegate to the binary, not print NOT IMPLEMENTED.
BLOB="$(mktemp)"

# Keep the run hermetic: without this the CLI creates and reads the *user's*
# real syscall registry under $XDG_DATA_HOME/$HOME, so the test both writes to
# the developer's machine and changes behaviour depending on what is already
# there. tests/harness/test_tempdir.hpp does the same for the C++ tests.
HYPERION_TEST_ROOT="$(mktemp -d)"
export XDG_DATA_HOME="${HYPERION_TEST_ROOT}/xdg-data"
export HOME="${HYPERION_TEST_ROOT}/home"
mkdir -p "$XDG_DATA_HOME" "$HOME"
trap 'rm -rf "$HYPERION_TEST_ROOT"; rm -f "$BLOB"' EXIT
printf '\xB8\x2a\x00\x00\x00\xF4' > "$BLOB" # mov eax, 42 ; hlt

out="$(bash "${WRAPPER}" run "$BLOB" --tier=0 --dump 2>&1)" \
    || fail "'run' exited non-zero"
if grep -q 'NOT IMPLEMENTED' <<<"$out"; then
    fail "'run' printed NOT IMPLEMENTED instead of delegating"
fi
grep -q '^status: ok$' <<<"$out" || fail "'run' did not report ok"
grep -q 'rax =0x000000000000002a' <<<"$out" || fail "'run' produced the wrong rax"

# Sanity: the wrapper is pointing at the binary we were handed.
[[ -x "$BIN" ]] || fail "test binary '$BIN' is not executable"

echo "wrapper ok: status honest, run delegates and executes"
