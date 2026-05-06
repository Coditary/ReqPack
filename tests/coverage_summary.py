#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def newest_coverage_xml(build_dir: Path) -> Path | None:
    matches = sorted(build_dir.glob("Testing/**/Coverage.xml"), key=lambda path: path.stat().st_mtime, reverse=True)
    return matches[0] if matches else None


def child_int(element: ET.Element, *names: str) -> int | None:
    wanted = {name.lower() for name in names}
    for child in element:
        if child.tag.lower() in wanted and child.text is not None:
            try:
                return int(child.text)
            except ValueError:
                return None
    return None


def child_float(element: ET.Element, *names: str) -> float | None:
    wanted = {name.lower() for name in names}
    for child in element:
        if child.tag.lower() in wanted and child.text is not None:
            try:
                return float(child.text)
            except ValueError:
                return None
    return None


def badge_color(coverage: float) -> str:
    if coverage >= 90.0:
        return "brightgreen"
    if coverage >= 80.0:
        return "green"
    if coverage >= 70.0:
        return "yellowgreen"
    if coverage >= 60.0:
        return "yellow"
    if coverage >= 50.0:
        return "orange"
    return "red"


def write_badge_json(path: Path, coverage: float) -> None:
    payload = {
        "schemaVersion": 1,
        "label": "coverage",
        "message": f"{coverage:.2f}%",
        "color": badge_color(coverage),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize CTest coverage for ReqPack sources")
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("source_dir", type=Path)
    parser.add_argument("--badge-json", type=Path, help="Write Shields endpoint JSON badge to this path")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    source_dir = args.source_dir.resolve()
    coverage_xml = newest_coverage_xml(build_dir)
    if coverage_xml is None:
        print(f"No Coverage.xml found under {build_dir}/Testing", file=sys.stderr)
        return 1

    tree = ET.parse(coverage_xml)
    root = tree.getroot()
    tracked_root = (source_dir / "src" / "main" / "cpp").resolve()
    rows: list[tuple[float, int, int, Path]] = []
    total_tested = 0
    total_count = 0

    for element in root.iter():
        if element.tag.lower() != "file":
            continue

        raw_path = element.attrib.get("FullPath") or element.attrib.get("fullpath") or element.attrib.get("Name") or element.attrib.get("name")
        if not raw_path:
            continue

        path = Path(raw_path)
        if not path.is_absolute():
            path = (source_dir / path).resolve()
        else:
            path = path.resolve()

        try:
            path.relative_to(tracked_root)
        except ValueError:
            continue

        loc_tested = child_int(element, "LOCTested", "LocTested")
        loc_untested = child_int(element, "LOCUnTested", "LOCUntested", "LocUnTested", "LocUntested")

        if loc_tested is None or loc_untested is None:
            percent = child_float(element, "PercentCoverage", "Percent")
            if percent is None:
                continue
            total = child_int(element, "Lines", "LOC")
            if total is None:
                continue
            loc_tested = round(total * percent / 100.0)
            loc_untested = total - loc_tested

        total = loc_tested + loc_untested
        if total <= 0:
            continue

        coverage = (loc_tested / total) * 100.0
        total_tested += loc_tested
        total_count += total
        rows.append((coverage, loc_tested, total, path))

    if not rows:
        print(f"Coverage XML found at {coverage_xml}, but no src/main/cpp file entries were parsed", file=sys.stderr)
        return 1

    rows.sort(key=lambda row: (row[0], str(row[3])))
    overall = (total_tested / total_count) * 100.0 if total_count else 0.0

    if args.badge_json is not None:
        write_badge_json(args.badge_json.resolve(), overall)

    print(f"Coverage summary: {overall:.2f}% ({total_tested}/{total_count} lines) across {len(rows)} source files")
    print(f"Coverage XML: {coverage_xml}")
    print("Lowest covered files:")
    for coverage, tested, total, path in rows[:10]:
        relative = path.relative_to(source_dir)
        print(f"  {coverage:6.2f}% ({tested:4d}/{total:4d})  {relative}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
