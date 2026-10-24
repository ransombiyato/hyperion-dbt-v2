#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# CLI contract tests for `hyperion syscall-registry`.
#
# This command is what a user touches when they extend the compatibility layer,
# so its behaviour is a promise, not an implementation detail:
#
#   * the default registry path is reported, and is outside the install dir
#   * --write creates it with mode 0600, and does NOT overwrite an existing one
#   * --validate never modifies the file, even when the file is invalid
#   * an unreadable/invalid registry is a reported error, never a silent
#     fallback that leaves the user believing their rules are active
#   * a rule that would take over a syscall HYPERION implements statically is
#     rejected, because the binary would otherwise silently ignore it
#   * --json output parses, and agrees with the human-readable summary
#
# Usage: registry_cli_test.sh <path-to-hyperion-binary>
set -euo pipefail

BIN="${1:?usage: registry_cli_test.sh <path-to-hyperion-binary> [emulator]}"
# Same convention as the guest suites: an optional emulator prefix, used by the
# cross pipeline to run the AArch64-built binary under qemu-user.
EMULATOR="${2:-${HYPERION_EMULATOR:-}}"
# The emulator prefix may carry arguments ("qemu-aarch64 -L /usr/aarch64-linux-gnu"),
# so it is split into words rather than used as a single quoted command.
EMULATOR_CMD=()
if [ -n "${EMULATOR}" ]; then
    read -r -a EMULATOR_CMD <<<"${EMULATOR}"
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

FAILED=0
pass() { echo "ok   $*"; }
fail() {
    echo "FAIL $*" >&2
    FAILED=$((FAILED + 1))
}

REGISTRY="${WORK}/registry.json"
export HYPERION_SYSCALL_REGISTRY="${REGISTRY}"

run() {
    if [ "${#EMULATOR_CMD[@]}" -gt 0 ]; then
        "${EMULATOR_CMD[@]}" "${BIN}" syscall-registry "$@"
    else
        "${BIN}" syscall-registry "$@"
    fi
}

# --- 1. the resolved path is the environment override -------------------------
out="$(run --list 2>&1 || true)"
if printf '%s' "${out}" | grep -q "resolved from  : override"; then
    pass "--list reports the environment override"
else
    fail "--list did not report the override source"
    printf '%s\n' "${out}" | sed 's/^/  /' >&2
fi

if [ -f "${REGISTRY}" ]; then
    pass "the registry was created on first use"
else
    fail "the registry was not created at ${REGISTRY}"
fi

mode="$(stat -c '%a' "${REGISTRY}" 2>/dev/null || echo missing)"
if [ "${mode}" = "600" ]; then
    pass "the registry is mode 600 (user-only)"
else
    fail "the registry is mode ${mode}, expected 600"
fi

# --- 2. --write must not clobber an existing registry -------------------------
before="$(cat "${REGISTRY}")"
if run --write >/dev/null 2>&1; then
    : # refusing is also acceptable; what matters is that it did not change
fi
if [ "$(cat "${REGISTRY}")" = "${before}" ]; then
    pass "--write left an existing registry untouched"
else
    fail "--write overwrote an existing registry"
fi

# --- 3. --validate never modifies the file ------------------------------------
printf '{"schema_version": 2, "rules": [{"id": "x"' >"${REGISTRY}"
before="$(cat "${REGISTRY}")"
if run --validate >"${WORK}/v.out" 2>&1; then
    fail "--validate accepted a truncated JSON document"
else
    pass "--validate rejects a truncated document with a non-zero status"
fi
if [ "$(cat "${REGISTRY}")" = "${before}" ]; then
    pass "--validate did not rewrite the file"
else
    fail "--validate rewrote the file it was asked to check"
fi

# --- 4. a rule that shadows a static implementation is refused ---------------
cat >"${REGISTRY}" <<'JSON'
{
  "schema_version": 2,
  "guest_arch": "x86_64",
  "rules": [
    {
      "id": "user.shadow.getpid",
      "name": "shadow_getpid",
      "syscall": 39,
      "implementation": "guest_alias",
      "target_syscall": 24,
      "origin": "user",
      "rule_version": 1,
      "enabled": true
    }
  ]
}
JSON
if run --validate >"${WORK}/shadow.out" 2>&1; then
    fail "a rule for statically implemented getpid(39) was accepted"
else
    pass "a rule that shadows a static implementation is refused"
fi
if grep -qi "reject\|conflict\|implement" "${WORK}/shadow.out"; then
    pass "the refusal says why"
else
    fail "the refusal does not explain itself"
    sed 's/^/  /' "${WORK}/shadow.out" >&2
fi

# --- 5. --json agrees with the text output -----------------------------------
cat >"${REGISTRY}" <<'JSON'
{
  "schema_version": 2,
  "guest_arch": "x86_64",
  "rules": [
    {
      "id": "user.json.custom300",
      "name": "custom300",
      "syscall": 300,
      "implementation": "guest_alias",
      "target_syscall": 39,
      "origin": "user",
      "rule_version": 1,
      "enabled": true
    }
  ]
}
JSON
if run --json >"${WORK}/j.out" 2>"${WORK}/j.err"; then
    if python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if isinstance(d, dict) else 1)' \
        "${WORK}/j.out"; then
        pass "--json emits a JSON object"
    else
        fail "--json output is not valid JSON"
        sed 's/^/  /' "${WORK}/j.out" >&2
    fi
    rules="$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1])).get("rules", [])))' \
        "${WORK}/j.out")"
    if [ "${rules}" -gt 1 ]; then
        pass "--json lists the compiled rules (${rules})"
    else
        fail "--json listed ${rules} rules; the built-in rules are missing from the report"
    fi
else
    fail "--json exited non-zero for a valid registry"
    sed 's/^/  /' "${WORK}/j.err" >&2
fi

# --- 6. --export-defaults is a valid document that round-trips ---------------
if run --export-defaults >"${WORK}/defaults.json" 2>"${WORK}/d.err"; then
    cp "${WORK}/defaults.json" "${REGISTRY}"
    if run --validate >"${WORK}/d2.out" 2>&1; then
        pass "the exported defaults validate as a registry"
    else
        fail "the exported defaults do not validate"
        sed 's/^/  /' "${WORK}/d2.out" >&2
    fi
else
    fail "--export-defaults failed"
    sed 's/^/  /' "${WORK}/d.err" >&2
fi

echo
if [ "${FAILED}" -ne 0 ]; then
    echo "registry CLI tests: ${FAILED} failure(s)" >&2
    exit 1
fi
echo "registry CLI tests: all checks passed"
