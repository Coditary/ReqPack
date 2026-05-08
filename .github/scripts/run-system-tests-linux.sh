#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <bundle-archive> <repo-root> <target>" >&2
    exit 1
fi

bundle_archive="$1"
repo_root="$2"
target="$3"

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

context_dir="$work_dir/context"
workspace_dir="$work_dir/workspace"
plugin_dir="$workspace_dir/plugins/demo-plugin"
report_path="$workspace_dir/plugin-test-report.json"
image_tag="reqpack-system-test:${target}"

mkdir -p "$context_dir" "$plugin_dir"
cp "$repo_root/Dockerfile.release" "$context_dir/Dockerfile"
cp "$bundle_archive" "$context_dir/rqp.tar.gz"
cp -R "$repo_root/tests/system/fixtures/demo-plugin/." "$plugin_dir/"

config_path="$("${repo_root}/.github/scripts/create-system-test-config.sh" "$workspace_dir" "$workspace_dir/plugins")"

volume_suffix=""
if command -v selinuxenabled >/dev/null 2>&1 && selinuxenabled; then
    volume_suffix=":Z"
fi

podman build -t "$image_tag" "$context_dir"

podman run --rm "$image_tag" --help >/dev/null
podman run --rm "$image_tag" version >/dev/null

podman run --rm \
    -v "$workspace_dir:/workspace${volume_suffix}" \
    "$image_tag" \
    --config /workspace/config.lua \
    test-plugin \
    --plugin /workspace/plugins/demo-plugin \
    --preset core \
    --report /workspace/plugin-test-report.json

grep -F '"failed": 0' "$report_path" >/dev/null
test -f "$config_path"
