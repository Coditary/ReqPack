#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


PERF_LINE = re.compile(r"^\s*(?P<percent>[0-9]+\.[0-9]+)%\s+(?P<command>\S+)\s+(?P<dso>\S+)\s+(?P<symbol>.+?)\s*$")
GPROF_LINE = re.compile(r"^\s*(?P<percent>[0-9]+\.[0-9]+)\s+(?P<cumulative>[0-9]+\.[0-9]+)\s+(?P<self>[0-9]+\.[0-9]+)\s+(?P<rest>.+?)\s*$")


def summarize_perf(report_path: Path) -> int:
    rows: list[tuple[float, str, str, str]] = []
    for line in report_path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = PERF_LINE.match(line)
        if not match:
            continue
        rows.append(
            (
                float(match.group("percent")),
                match.group("command"),
                match.group("dso"),
                match.group("symbol"),
            )
        )

    if not rows:
        print(f"No perf hotspots parsed from {report_path}", file=sys.stderr)
        return 1

    print(f"Hotspot summary from {report_path}:")
    for percent, command, dso, symbol in rows[:10]:
        print(f"  {percent:6.2f}%  {command:<22} {dso:<24} {symbol}")
    return 0


def summarize_gprof(report_path: Path) -> int:
    rows: list[tuple[float, str]] = []
    in_table = False

    for line in report_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.strip().startswith("time") and "name" in line:
            in_table = True
            continue
        if not in_table:
            continue
        if not line.strip():
            if rows:
                break
            continue

        match = GPROF_LINE.match(line)
        if not match:
            continue

        parts = match.group("rest").split()
        if not parts:
            continue
        rows.append((float(match.group("percent")), parts[-1]))

    if not rows:
        print(f"No gprof hotspots parsed from {report_path}", file=sys.stderr)
        return 1

    print(f"Hotspot summary from {report_path}:")
    for percent, symbol in rows[:10]:
        print(f"  {percent:6.2f}%  {symbol}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize ReqPack profiling reports")
    parser.add_argument("mode", choices=["perf", "gprof"])
    parser.add_argument("report_path", type=Path)
    args = parser.parse_args()

    report_path = args.report_path.resolve()
    if not report_path.is_file():
        print(f"Report file not found: {report_path}", file=sys.stderr)
        return 1

    if args.mode == "perf":
        return summarize_perf(report_path)
    return summarize_gprof(report_path)


if __name__ == "__main__":
    raise SystemExit(main())
