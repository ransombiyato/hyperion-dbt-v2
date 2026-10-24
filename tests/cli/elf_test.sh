#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# ELF-path CLI test.
#
# `hyperion info` must describe a real ELF64 image, and `hyperion run` must load
# it from its program headers and execute it. The fixture is a hand-built static
# ET_EXEC whose only instruction is `hlt` at its entry point (0x401000), so a
# successful run is exactly one retired instruction with rip at the entry.
#
# This is the ELF analogue of cli_smoke.sh and, like it, works on both an x86-64
# host (tier 1 falls back) and an AArch64 host / qemu-user (tier 1 runs generated
# code). It also checks the two failure paths a user will hit: a non-ELF file
# must be treated as a raw blob (not mis-parsed), and a truncated ELF must be
# rejected with a clear reason rather than crashing.
#
# Usage: elf_test.sh <path-to-hyperion-binary> <path-to-fixture-elf>
set -euo pipefail

BIN="${1:?usage: elf_test.sh <hyperion-binary> <fixture.elf>}"
FIXTURE="${2:?usage: elf_test.sh <hyperion-binary> <fixture.elf>}"
EMULATOR="${HYPERION_EMULATOR:-}"

TMP="$(mktemp -d)"

# Keep the run hermetic: without this the CLI creates and reads the *user's*
# real syscall registry under $XDG_DATA_HOME/$HOME, so the test both writes to
# the developer's machine and changes behaviour depending on what is already
# there. tests/harness/test_tempdir.hpp does the same for the C++ tests.
HYPERION_TEST_ROOT="$(mktemp -d)"
export XDG_DATA_HOME="${HYPERION_TEST_ROOT}/xdg-data"
export HOME="${HYPERION_TEST_ROOT}/home"
mkdir -p "$XDG_DATA_HOME" "$HOME"
trap 'rm -rf "$HYPERION_TEST_ROOT" "$TMP"' EXIT

# shellcheck disable=SC2086
run() { $EMULATOR "$BIN" "$@"; }

# --- info -------------------------------------------------------------------
info="$(run info "$FIXTURE")"
grep -q '^type:.*static executable' <<<"$info" || { echo "info: not static" >&2; echo "$info" >&2; exit 1; }
grep -q '^entry:       0x401000$' <<<"$info" || { echo "info: bad entry" >&2; echo "$info" >&2; exit 1; }
grep -q '^segments:    1 PT_LOAD$' <<<"$info" || { echo "info: bad segment count" >&2; echo "$info" >&2; exit 1; }

# --- run, both tiers --------------------------------------------------------
for tier in 0 1; do
    out="$(run run "$FIXTURE" "--tier=$tier" --max-steps=1000 2>"$TMP/err")"
    if ! grep -q '^loaded ELF at entry 0x401000' <<<"$out"; then
        echo "tier $tier: image not loaded" >&2
        echo "$out" >&2
        exit 1
    fi
    if ! grep -q '^status: ok$' <<<"$out"; then
        echo "tier $tier: not ok" >&2
        echo "$out" >&2
        exit 1
    fi
    if ! grep -q '^steps:  1$' <<<"$out"; then
        echo "tier $tier: expected exactly one retired instruction" >&2
        echo "$out" >&2
        exit 1
    fi
done

# --- a non-ELF file is still treated as a raw blob --------------------------
# The hlt byte alone, with no ELF magic, must go down the blob path.
printf '\xF4' > "$TMP/blob.bin"
blob_out="$(run run "$TMP/blob.bin" --tier=0 --max-steps=10)"
if grep -q 'loaded ELF' <<<"$blob_out"; then
    echo "a non-ELF file was mis-parsed as an ELF" >&2
    exit 1
fi
grep -q '^status: ok$' <<<"$blob_out" || { echo "blob path regressed" >&2; echo "$blob_out" >&2; exit 1; }

# --- a truncated ELF is rejected, not crashed -------------------------------
head -c 40 "$FIXTURE" > "$TMP/trunc.elf"
if run info "$TMP/trunc.elf" >"$TMP/trunc.out" 2>&1; then
    echo "a truncated ELF was accepted by info" >&2
    cat "$TMP/trunc.out" >&2
    exit 1
fi
grep -q "shorter than an ELF64 header" "$TMP/trunc.out" || {
    echo "truncated ELF gave an unclear error" >&2
    cat "$TMP/trunc.out" >&2
    exit 1
}

echo "elf cli ok: info describes the image, both tiers run it, bad inputs are rejected"
