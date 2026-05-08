#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <bundle-archive> <repo-root>" >&2
    exit 1
fi

bundle_archive="$1"
repo_root="$2"

[ -f "$bundle_archive" ] || {
    echo "release bundle missing: $bundle_archive" >&2
    exit 1
}

[ -d "$repo_root/tests/system/fixtures/demo-plugin" ] || {
    echo "system-test fixture missing under tests/system/fixtures/demo-plugin" >&2
    exit 1
}

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT INT TERM HUP

bundle_dir="$work_dir/bundle"
workspace_dir="$work_dir/workspace"
plugin_dir="$workspace_dir/plugins/demo-plugin"
report_path="$workspace_dir/plugin-test-report.json"

mkdir -p "$bundle_dir" "$plugin_dir"
tar -xzf "$bundle_archive" -C "$bundle_dir"
cp -R "$repo_root/tests/system/fixtures/demo-plugin/." "$plugin_dir/"

config_path="$("${repo_root}/.github/scripts/create-system-test-config.sh" "$workspace_dir" "$workspace_dir/plugins")"

test -x "$bundle_dir/rqp"
"$bundle_dir/rqp" --help >/dev/null
"$bundle_dir/rqp" version >/dev/null
"$bundle_dir/rqp" --config "$config_path" test-plugin --plugin "$plugin_dir" --preset core --report "$report_path"

grep -F '"failed": 0' "$report_path" >/dev/null
