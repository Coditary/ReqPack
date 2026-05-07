#!/usr/bin/env sh

set -eu

RELEASE_REPO_URL="${REQPACK_RELEASE_REPO_URL:-https://github.com/Coditary/ReqPack.git}"
RELEASE_API_BASE_URL="${REQPACK_RELEASE_API_BASE_URL:-https://api.github.com}"
RELEASE_TAG="${REQPACK_RELEASE_TAG:-latest}"
INSTALL_DIR="${REQPACK_INSTALL_DIR:-$HOME/.local/bin}"
SELF_BIN_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/reqpack/self/bin"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/reqpack"
CONFIG_PATH="$CONFIG_DIR/config.lua"

need_cmd() {
  command -v "$1" >/dev/null 2>&1
}

die() {
  printf '%s\n' "$1" >&2
  exit 1
}

trim_trailing_slash() {
  value="$1"
  while [ -n "$value" ] && [ "${value%/}" != "$value" ]; do
    value=${value%/}
  done
  printf '%s' "$value"
}

parse_owner_repo() {
  value="$1"
  case "$value" in
    *://*)
      path=${value#*://}
      path=${path#*/}
      ;;
    *@*:*)
      path=${value#*:}
      ;;
    *)
      path=$value
      ;;
  esac
  path=${path%.git}
  path=${path#/}
  repo=${path##*/}
  rest=${path%/*}
  owner=${rest##*/}
  [ -n "$owner" ] || return 1
  [ -n "$repo" ] || return 1
  printf '%s %s\n' "$owner" "$repo"
}

detect_target() {
  os=$(uname -s)
  arch=$(uname -m)

  case "$os" in
    Linux) target_os="linux" ;;
    Darwin) target_os="darwin" ;;
    *) die "Unsupported operating system: $os" ;;
  esac

  case "$arch" in
    x86_64|amd64) target_arch="x86_64" ;;
    aarch64|arm64) target_arch="aarch64" ;;
    *) die "Unsupported architecture: $arch" ;;
  esac

  printf '%s-%s\n' "$target_arch" "$target_os"
}

release_api_url() {
  api_base=$(trim_trailing_slash "$1")
  owner="$2"
  repo="$3"
  tag="$4"

  if [ "$tag" = "latest" ]; then
    printf '%s/repos/%s/%s/releases/latest\n' "$api_base" "$owner" "$repo"
  else
    printf '%s/repos/%s/%s/releases/tags/%s\n' "$api_base" "$owner" "$repo" "$tag"
  fi
}

extract_json_field() {
  python3 - "$1" "$2" <<'PY'
import json, sys
path, field = sys.argv[1:3]
with open(path, 'r', encoding='utf-8') as handle:
    data = json.load(handle)
value = data.get(field, '')
if isinstance(value, str):
    sys.stdout.write(value)
PY
}

resolve_asset_url() {
  python3 - "$1" "$2" <<'PY'
import json, sys
path, asset_name = sys.argv[1:3]
with open(path, 'r', encoding='utf-8') as handle:
    data = json.load(handle)
for asset in data.get('assets', []):
    if asset.get('name') == asset_name:
        sys.stdout.write(asset.get('browser_download_url', ''))
        break
PY
}

ensure_linux_tools() {
  if need_cmd curl && need_cmd tar && need_cmd python3 && need_cmd git; then
    return 0
  fi
  if need_cmd apt-get; then
    sudo apt-get install -y --no-install-recommends \
      ca-certificates curl git python3 tar
    return 0
  fi
  die "Missing curl/tar/python3/git. Install them manually, then rerun install.sh."
}

ensure_macos_tools() {
  if need_cmd curl && need_cmd tar && need_cmd python3 && need_cmd git; then
    return 0
  fi
  if need_cmd brew; then
    brew install curl git python3
    return 0
  fi
  die "Missing curl/tar/python3/git. Install Command Line Tools or Homebrew, then rerun install.sh."
}

ensure_tools() {
  case "$(uname -s)" in
    Linux) ensure_linux_tools ;;
    Darwin) ensure_macos_tools ;;
    *) die "Unsupported operating system: $(uname -s)" ;;
  esac
}

write_default_config() {
  owner="$1"
  repo="$2"

  mkdir -p "$CONFIG_DIR"
  if [ -f "$CONFIG_PATH" ]; then
    return 0
  fi

  cat >"$CONFIG_PATH" <<EOF
return {
  registry = {
    remoteUrl = "https://github.com/Coditary/rqp-registry.git",
  },
  selfUpdate = {
    repoUrl = "https://github.com/$owner/$repo.git",
    releaseApiBaseUrl = "$(trim_trailing_slash "$RELEASE_API_BASE_URL")",
    releaseTag = "latest",
    linkPath = "$INSTALL_DIR/rqp",
  },
}
EOF
}

main() {
  ensure_tools

  if ! repo_parts=$(parse_owner_repo "$RELEASE_REPO_URL"); then
    die "REQPACK_RELEASE_REPO_URL must point to GitHub repository."
  fi
  owner=${repo_parts%% *}
  repo=${repo_parts#* }
  target=$(detect_target)

  tmpdir=$(mktemp -d)
  trap 'rm -rf "$tmpdir"' EXIT INT TERM HUP

  metadata_path="$tmpdir/release.json"
  archive_path="$tmpdir/rqp.tar.gz"
  extract_path="$tmpdir/extract"

  curl -fsSL \
    -H 'Accept: application/vnd.github+json' \
    -H 'X-GitHub-Api-Version: 2022-11-28' \
    -A 'ReqPack install.sh' \
    -o "$metadata_path" \
    "$(release_api_url "$RELEASE_API_BASE_URL" "$owner" "$repo" "$RELEASE_TAG")"

  tag_name=$(extract_json_field "$metadata_path" tag_name)
  [ -n "$tag_name" ] || die "Could not resolve release tag from release metadata."

  asset_name="rqp-$tag_name-$target.tar.gz"
  asset_url=$(resolve_asset_url "$metadata_path" "$asset_name")
  [ -n "$asset_url" ] || die "No release asset for target $target in release $tag_name."

  bundle_path="$SELF_BIN_DIR/rqp-$tag_name-$target"

  mkdir -p "$extract_path" "$INSTALL_DIR" "$SELF_BIN_DIR"
  curl -fsSL -A 'ReqPack install.sh' -o "$archive_path" "$asset_url"
  tar -xzf "$archive_path" -C "$extract_path"

  binary_path="$extract_path/rqp"
  [ -f "$binary_path" ] || die "Release archive does not contain rqp binary at archive root."

  chmod +x "$binary_path"
  rm -rf "$bundle_path"
  mkdir -p "$bundle_path"
  cp -R "$extract_path"/. "$bundle_path"/
  chmod +x "$bundle_path/rqp"
  if [ -f "$bundle_path/bin/rqp.bin" ]; then
    chmod +x "$bundle_path/bin/rqp.bin"
  fi
  rm -f "$INSTALL_DIR/rqp"
  ln -s "$bundle_path/rqp" "$INSTALL_DIR/rqp"
  write_default_config "$owner" "$repo"

  printf 'Installed ReqPack %s to %s/rqp\n' "$tag_name" "$INSTALL_DIR"
  "$INSTALL_DIR/rqp" update --all || printf '%s\n' 'Warning: rqp update --all failed. Run it manually after checking your environment.' >&2
  "$INSTALL_DIR/rqp" host refresh || printf '%s\n' 'Warning: rqp host refresh failed. Run it manually if plugins need fresh host info.' >&2

  case ":$PATH:" in
    *":$INSTALL_DIR:"*) ;;
    *) printf 'Add %s to PATH if needed.\n' "$INSTALL_DIR" ;;
  esac
}

main "$@"
