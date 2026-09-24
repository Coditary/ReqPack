#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

TRACKED_PREFIXES = (
    "src/main/cpp/cli/",
    "src/main/cpp/output/",
)

DEFAULT_EXCLUDE_SUFFIXES = (
    "src/main/cpp/cli/cli_help_text.cpp",
    "src/main/cpp/cli/cli_parse_core.cpp",
    "src/main/cpp/main.cpp",
    "src/main/cpp/main_dispatch.cpp",
    "src/main/cpp/main_self_update.cpp",
    "src/main/cpp/main_self_update_release.cpp",
    "src/main/cpp/main_self_update_support.cpp",
    "src/main/cpp/main_stdin.cpp",
    "src/main/cpp/main_diagnostics.cpp",
)


def badge_color(coverage: float) -> str:
    if coverage >= 90.0:
        return "brightgreen"
    if coverage >= 85.0:
        return "green"
    if coverage >= 80.0:
        return "yellowgreen"
    if coverage >= 70.0:
        return "yellow"
    if coverage >= 60.0:
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


def is_excluded(relative_path: str, exclude_suffixes: tuple[str, ...]) -> bool:
    normalized = relative_path.replace("\\", "/")
    return any(normalized.endswith(suffix) or suffix in normalized for suffix in exclude_suffixes)


def normalize_gcovr_filename(source_dir: Path, filename: str) -> str | None:
    normalized = filename.replace("\\", "/")
    if normalized.startswith("src/main/cpp/"):
        return normalized
    marker = "/reqpack/src/main/cpp/"
    if marker in normalized:
        return "src/main/cpp/" + normalized.split(marker, 1)[1]
    for marker in (
        "/reqpack-security-core/src/",
        "/reqpack_security_core-src/src/",
        "_deps/reqpack_security_core-src/src/",
    ):
        if marker in normalized:
            return "../reqpack-security-core/src/" + normalized.split(marker, 1)[1]
    if normalized.startswith("../reqpack-security-core/src/"):
        return normalized
    return None


def coverage_object_directories(build_dir: Path) -> list[Path]:
    directories = [build_dir]
    for relative in (
        "reqpack-security-core-build",
        "_deps/reqpack_security_core-build",
        "ReqPack-Core-build",
        "_deps/reqpack_core-build",
    ):
        candidate = build_dir / relative
        if candidate.is_dir():
            directories.append(candidate)
    return directories


def is_tracked(relative_path: str, source_dir: Path) -> bool:
    if relative_path.startswith("../reqpack-security-core/src/"):
        return True
    return any(relative_path.startswith(prefix) for prefix in TRACKED_PREFIXES)


def run_gcovr_summary(build_dir: Path, source_dir: Path, exclude_suffixes: tuple[str, ...]) -> tuple[float, int, int, list[tuple[float, int, int, str]]]:
    object_directories = coverage_object_directories(build_dir)

    command = [
        "gcovr",
        "-r",
        str(source_dir),
        "--gcov-ignore-parse-errors",
        "all",
        "--json-summary-pretty",
    ]
    for object_directory in object_directories:
        command.extend(["--object-directory", str(object_directory)])
    command.extend(
        [
            "--exclude",
            "build/.*",
            "--exclude",
            "tests/.*",
            "--exclude",
            "_deps/.*",
            "--exclude",
            ".*/ReqPack-Core/.*",
            "--exclude",
            ".*/reqpack_core/.*",
        ]
    )

    result = subprocess.run(command, check=False, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or "gcovr failed")

    payload = json.loads(result.stdout)
    rows: list[tuple[float, int, int, str]] = []
    total_covered = 0
    total_count = 0

    for entry in payload.get("files", []):
        relative = normalize_gcovr_filename(source_dir, entry.get("filename", ""))
        if relative is None or not is_tracked(relative, source_dir):
            continue
        if is_excluded(relative, exclude_suffixes):
            continue

        covered = int(entry.get("line_covered", 0))
        total = int(entry.get("line_total", 0))
        if total <= 0:
            continue

        coverage = (covered / total) * 100.0
        total_covered += covered
        total_count += total
        rows.append((coverage, covered, total, relative))

    if not rows:
        raise RuntimeError("gcovr completed, but no tracked source file entries were parsed")

    rows.sort(key=lambda row: (row[0], row[3]))
    overall = (total_covered / total_count) * 100.0 if total_count else 0.0
    return overall, total_covered, total_count, rows


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize ReqPack CLI coverage from merged gcov data")
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("source_dir", type=Path)
    parser.add_argument("--badge-json", type=Path, help="Write Shields endpoint JSON badge to this path")
    parser.add_argument(
        "--exclude",
        action="append",
        default=[],
        help="Relative path suffix to exclude from coverage totals (may be repeated)",
    )
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    source_dir = args.source_dir.resolve()
    exclude_suffixes = tuple(DEFAULT_EXCLUDE_SUFFIXES) + tuple(args.exclude)

    try:
        overall, total_covered, total_count, rows = run_gcovr_summary(build_dir, source_dir, exclude_suffixes)
    except FileNotFoundError:
        print("gcovr is required for coverage summaries but was not found on PATH", file=sys.stderr)
        return 1
    except RuntimeError as error:
        print(str(error), file=sys.stderr)
        return 1

    if args.badge_json is not None:
        write_badge_json(args.badge_json.resolve(), overall)

    print(f"Coverage summary: {overall:.2f}% ({total_covered}/{total_count} lines) across {len(rows)} source files")
    print(f"Coverage build: {build_dir}")
    print("Lowest covered files:")
    for coverage, tested, total, relative in rows[:10]:
        print(f"  {coverage:6.2f}% ({tested:4d}/{total:4d})  {relative}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
