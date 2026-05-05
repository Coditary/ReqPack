#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 4 ]; then
    echo "usage: $0 <build-dir> <tag> <target> <output-dir>" >&2
    exit 1
fi

build_dir="$1"
tag="$2"
target="$3"
output_dir="$4"
binary_path="${build_dir%/}/ReqPack"
archive_root="rqp-${tag}-${target}"

if [ ! -f "$binary_path" ]; then
    echo "built binary missing: $binary_path" >&2
    exit 1
fi

mkdir -p "$output_dir"
staging_dir="$(mktemp -d)"
trap 'rm -rf "$staging_dir"' EXIT

mkdir -p "$staging_dir/$archive_root"
cp "$binary_path" "$staging_dir/$archive_root/rqp"
chmod +x "$staging_dir/$archive_root/rqp"
cp README.md "$staging_dir/$archive_root/README.md"

license_copied="0"
for candidate in LICENSE LICENCE LICENSE.md LICENCE.md; do
    if [ -f "$candidate" ]; then
        cp "$candidate" "$staging_dir/$archive_root/$(basename "$candidate")"
        license_copied="1"
        break
    fi
done

if [ "$license_copied" != "1" ]; then
    echo "warning: no root license file found" >&2
fi

tar -C "$staging_dir" -czf "${output_dir%/}/${archive_root}.tar.gz" "$archive_root"
