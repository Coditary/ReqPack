#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <binary-path> <repo-root> <msys-prefix-path>" >&2
    exit 1
fi

binary_path="$1"
repo_root="$2"
msys_prefix="$3"

[ -x "$binary_path" ] || {
    echo "rqp binary missing or not executable: $binary_path" >&2
    exit 1
}

[ -d "$repo_root/tests/system/fixtures/demo-plugin" ] || {
    echo "system-test fixture missing under tests/system/fixtures/demo-plugin" >&2
    exit 1
}

export PATH="${msys_prefix}/bin:${PATH}"

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT INT TERM HUP

workspace_dir="$work_dir/workspace"
plugin_dir="$workspace_dir/plugins/demo-plugin"
report_path="$workspace_dir/plugin-test-report.json"

mkdir -p "$plugin_dir"
cp -R "$repo_root/tests/system/fixtures/demo-plugin/." "$plugin_dir/"

config_path="$("${repo_root}/.github/scripts/create-system-test-config.sh" "$workspace_dir" "$workspace_dir/plugins")"

"$binary_path" --help >/dev/null
"$binary_path" version >/dev/null
"$binary_path" --config "$config_path" test-plugin --plugin "$plugin_dir" --preset core --report "$report_path"

grep -F '"failed": 0' "$report_path" >/dev/null
