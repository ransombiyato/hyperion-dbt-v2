#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate and validate the Hyperion syscall inventory.

Two responsibilities:

  validate  Cross-check docs/syscalls/inventory.tsv against the pinned kernel
            table (docs/syscalls/kernel-table-x86_64-6.12.tsv) and report any
            missing, extra, duplicated, misnumbered or mis-tiered entry. Exit
            non-zero on any error so CI fails.

  generate  (default) Regenerate docs/syscalls/U1..U4.md and E1..E3.md from the
            canonical inventory.tsv. The generated files carry a banner saying
            so; edit inventory.tsv, not the Markdown.

The canonical data source is inventory.tsv. The Markdown files are readable
documentation derived from it. Nothing here invents frequency statistics: the
`evidence` column records either a strace count (trace:N) or a reason.
"""
from __future__ import annotations

import argparse
import collections
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SYSDIR = ROOT / "docs" / "syscalls"
INVENTORY = SYSDIR / "inventory.tsv"
KERNEL_TABLE = SYSDIR / "kernel-table-x86_64-6.12.tsv"
SYSCALL_HPP = ROOT / "include" / "hyperion" / "syscall.hpp"
SYSCALL_CPP = ROOT / "src" / "core" / "syscall.cpp"
# The compatibility-adapter layer implements further syscalls outside the
# original switch. It is scanned too, so "implemented in C++" means what the
# whole handler implements rather than one file of it.
SYSCALL_ADAPTERS_CPP = ROOT / "src" / "core" / "syscall_adapters.cpp"

USAGE_TIERS = ["U4", "U3", "U2", "U1"]
EXTRA_TIERS = ["E1", "E2", "E3"]
ALL_TIERS = USAGE_TIERS + EXTRA_TIERS

TIER_TITLE = {
    "U4": "U4 — Most frequently used",
    "U3": "U3 — Frequently used",
    "U2": "U2 — Less frequently used",
    "U1": "U1 — Least frequently used (usage tier)",
    "E1": "E1 — High-value additional syscalls",
    "E2": "E2 — Specialized extensions",
    "E3": "E3 — Rare, legacy, privileged or unusually complex",
}

STATUS_VALUES = {"not-started", "stubbed", "implemented", "validated"}

BANNER = (
    "<!-- GENERATED FILE - do not edit by hand.\n"
    "     Source: docs/syscalls/inventory.tsv\n"
    "     Regenerate: python3 tools/gen_syscall_inventory.py\n"
    "     Validate:   python3 tools/gen_syscall_inventory.py --check -->\n"
)


class Row:
    __slots__ = ("nr", "name", "tier", "status", "purpose", "evidence", "lineno")

    def __init__(self, nr, name, tier, status, purpose, evidence, lineno):
        self.nr = nr
        self.name = name
        self.tier = tier
        self.status = status
        self.purpose = purpose
        self.evidence = evidence
        self.lineno = lineno


def read_kernel_table():
    """Return {nr: (abi, name)} for every entry in the pinned table."""
    table = {}
    for line in KERNEL_TABLE.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        nr, abi, name = parts[0], parts[1], parts[2]
        table[int(nr)] = (abi, name)
    return table


def read_inventory():
    rows = []
    errors = []
    for i, line in enumerate(INVENTORY.read_text().splitlines(), start=1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 6:
            errors.append(f"{INVENTORY}:{i}: expected 6 tab-separated columns, got {len(parts)}")
            continue
        nr, name, tier, status, purpose, evidence = parts
        if not nr.isdigit():
            errors.append(f"{INVENTORY}:{i}: syscall number {nr!r} is not an integer")
            continue
        rows.append(Row(int(nr), name, tier, status, purpose, evidence, i))
    return rows, errors


def validate(rows, kernel, verbose=True):
    errors = []
    warnings = []

    by_nr = collections.defaultdict(list)
    by_name = collections.defaultdict(list)
    for r in rows:
        by_nr[r.nr].append(r)
        by_name[r.name].append(r)

    # 1. duplicates
    for nr, group in by_nr.items():
        if len(group) > 1:
            errors.append(f"duplicate syscall number {nr}: lines {[g.lineno for g in group]}")
    for name, group in by_name.items():
        if len(group) > 1:
            errors.append(f"duplicate syscall name {name!r}: lines {[g.lineno for g in group]}")

    # 2. number/name agreement with the kernel table
    for r in rows:
        if r.nr not in kernel:
            errors.append(f"{INVENTORY}:{r.lineno}: number {r.nr} is not in the pinned kernel table")
            continue
        abi, kname = kernel[r.nr]
        if abi == "x32":
            errors.append(f"{INVENTORY}:{r.lineno}: number {r.nr} is an x32 ABI entry; "
                          f"the inventory is common/64 only")
        if kname != r.name:
            errors.append(f"{INVENTORY}:{r.lineno}: number {r.nr} is {kname!r} in the kernel "
                          f"table but {r.name!r} here")

    # 3. tier and status validity
    for r in rows:
        if r.tier not in ALL_TIERS:
            errors.append(f"{INVENTORY}:{r.lineno}: invalid tier {r.tier!r}")
        if r.status not in STATUS_VALUES:
            errors.append(f"{INVENTORY}:{r.lineno}: invalid status {r.status!r}")

    # 4. completeness: every common/64 syscall must be present exactly once
    common64 = {nr for nr, (abi, _) in kernel.items() if abi in ("common", "64")}
    present = set(by_nr)
    missing = sorted(common64 - present)
    extra = sorted(present - common64)
    for nr in missing:
        errors.append(f"missing: syscall {nr} ({kernel[nr][1]}) is in the kernel table "
                      f"but absent from the inventory")
    for nr in extra:
        errors.append(f"extra: syscall {nr} is in the inventory but not common/64 in the "
                      f"kernel table")

    # 4b. syscalls reachable only through the compatibility layer must be
    #     implemented too: the adapter table and the alias rules are scanned so
    #     a syscall cannot be marked implemented in the inventory without a real
    #     handler somewhere, and an adapter cannot exist unnoticed by the
    #     inventory.
    adapter_nrs = set()
    alias_nrs = set()
    if SYSCALL_ADAPTERS_CPP.exists():
        adapters_text = SYSCALL_ADAPTERS_CPP.read_text()
        # Contract with src/core/syscall_adapters.cpp: every row of
        # `kAdapterTable` begins with {"<handler-id>", <syscall-number>, and every
        # shipped alias rule assigns `rule.syscall = <number>;`.
        adapter_nrs = {int(m.group(2)) for m in re.finditer(
            r'\{"(compat\.[a-z0-9_]+)",\s*(\d+),', adapters_text)}
        alias_nrs = {int(m.group(1)) for m in re.finditer(
            r"rule\.syscall = (\d+);", adapters_text)}
        if not adapter_nrs:
            errors.append("C++ sync: no adapters found in src/core/syscall_adapters.cpp "
                          "(the scanner and the table have drifted apart)")

    # 5. bidirectional C++ implementation sync: every `case sysno::kX:` handled
    #    in MinimalSyscallHandler::handle() and listed in is_minimal_syscall()
    #    must match `status != 'not-started'` in inventory.tsv.
    if SYSCALL_HPP.exists() and SYSCALL_CPP.exists():
        hpp_text = SYSCALL_HPP.read_text()
        cpp_text = SYSCALL_CPP.read_text()
        sysno_map = {
            m.group(1): int(m.group(2))
            for m in re.finditer(
                r"inline\s+constexpr\s+std::uint64_t\s+(k\w+)\s*=\s*(\d+)\s*;", hpp_text
            )
        }
        handle_body_match = re.search(
            r"MinimalSyscallHandler::handle\s*\([^)]*\)\s*\{(.*?)\nbool\s+is_minimal_syscall",
            cpp_text,
            re.DOTALL,
        )
        minimal_body_match = re.search(
            r"bool\s+is_minimal_syscall\s*\([^)]*\)\s*\{(.*?)\n\}",
            cpp_text,
            re.DOTALL,
        )
        if handle_body_match and minimal_body_match:
            handled_syms = set(
                re.findall(r"\bcase\s+sysno::(k\w+)\s*:", handle_body_match.group(1))
            )
            minimal_syms = set(
                re.findall(r"\bsysno::(k\w+)\b", minimal_body_match.group(1))
            )
            for sym in sorted(handled_syms ^ minimal_syms):
                errors.append(
                    f"C++ sync: sysno::{sym} differs between MinimalSyscallHandler::handle() "
                    f"and is_minimal_syscall()"
                )
            cpp_implemented_nrs = {sysno_map[s] for s in handled_syms if s in sysno_map}
            cpp_implemented_nrs |= adapter_nrs | alias_nrs
            tsv_implemented_nrs = {r.nr for r in rows if r.status != "not-started"}
            for nr in sorted(cpp_implemented_nrs - tsv_implemented_nrs):
                errors.append(
                    f"C++ -> TSV drift: syscall {nr} ({kernel.get(nr, ('?', '?'))[1]}) is "
                    f"handled in src/core/syscall.cpp but marked 'not-started' in inventory.tsv"
                )
            for nr in sorted(tsv_implemented_nrs - cpp_implemented_nrs):
                errors.append(
                    f"TSV -> C++ drift: syscall {nr} ({kernel.get(nr, ('?', '?'))[1]}) is "
                    f"marked implemented in inventory.tsv but not handled in src/core/syscall.cpp"
                )

    if verbose:
        counts = collections.Counter(r.tier for r in rows)
        implemented_count = sum(1 for r in rows if r.status != "not-started")
        print(f"inventory entries : {len(rows)}")
        print(f"kernel common/64  : {len(common64)}")
        print(f"implemented       : {implemented_count}")
        for t in ALL_TIERS:
            print(f"  {t}: {counts.get(t, 0)}")
        print(f"errors            : {len(errors)}")
        print(f"warnings          : {len(warnings)}")
        for e in errors:
            print(f"  ERROR: {e}")
        for w in warnings:
            print(f"  WARN:  {w}")
    return errors, warnings


def evidence_label(ev: str) -> str:
    if ev.startswith("trace:"):
        return f"trace, {ev.split(':', 1)[1]} calls"
    if ev.startswith("abi:"):
        return "ABI requirement"
    return "provisional"


def render_tier(tier, rows):
    rows = sorted(rows, key=lambda r: r.nr)
    out = [BANNER, f"# Syscall inventory — {TIER_TITLE[tier]}", ""]
    out.append(f"Entries: **{len(rows)}**. This file is generated from "
               "`docs/syscalls/inventory.tsv`; see `docs/syscalls/README.md` for "
               "tier criteria and the evidence policy.")
    out.append("")
    out.append("| nr | name | status | evidence | purpose |")
    out.append("| ---: | --- | --- | --- | --- |")
    for r in rows:
        out.append(f"| {r.nr} | `{r.name}` | {r.status} | {evidence_label(r.evidence)} | {r.purpose} |")
    out.append("")
    return "\n".join(out)


def generate(rows):
    written = []
    for tier in ALL_TIERS:
        tier_rows = [r for r in rows if r.tier == tier]
        text = render_tier(tier, tier_rows)
        path = SYSDIR / f"{tier}.md"
        path.write_text(text)
        written.append((path, len(tier_rows)))
    return written


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="validate only; do not write files (used by CI)")
    args = ap.parse_args(argv)

    if not KERNEL_TABLE.exists():
        print(f"error: pinned kernel table missing: {KERNEL_TABLE}", file=sys.stderr)
        return 2

    kernel = read_kernel_table()
    rows, parse_errors = read_inventory()

    print("== validating inventory ==")
    errors, _ = validate(rows, kernel)
    errors = parse_errors + errors

    if errors:
        print(f"\nFAILED: {len(errors)} error(s); not writing any files.", file=sys.stderr)
        return 1

    if args.check:
        stale = []
        for tier in ALL_TIERS:
            tier_rows = [r for r in rows if r.tier == tier]
            expected = render_tier(tier, tier_rows)
            path = SYSDIR / f"{tier}.md"
            if not path.exists() or path.read_text() != expected:
                stale.append(str(path.relative_to(ROOT)))
        if stale:
            print(
                f"\nFAILED: generated Markdown out of date ({', '.join(stale)}); "
                f"re-run: python3 tools/gen_syscall_inventory.py",
                file=sys.stderr,
            )
            return 1
        print("\nOK: inventory, C++ handler, and generated Markdown are complete and in sync.")
        return 0

    written = generate(rows)
    print("\n== generated ==")
    for path, n in written:
        print(f"  {path.relative_to(ROOT)}  ({n} entries)")
    print("\nOK: inventory valid and regenerated.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
