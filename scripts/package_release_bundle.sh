#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 3 ]; then
  printf 'usage: %s <binary-path> <output-archive> <linux|macos>\n' "$0" >&2
  exit 1
fi

binary_path="$1"
output_archive="$2"
target_family="$3"

need_cmd() {
  command -v "$1" >/dev/null 2>&1
}

die() {
  printf '%s\n' "$1" >&2
  exit 1
}

copy_following_symlink() {
  src="$1"
  dest="$2"
  mkdir -p "$(dirname "$dest")"
  cp -L "$src" "$dest"
  chmod u+w "$dest" 2>/dev/null || true
}

write_launcher() {
  cat >"$bundle_dir/rqp" <<'EOF'
#!/usr/bin/env sh
set -eu

resolve_script_path() {
  target="$1"
  while [ -L "$target" ]; do
    dir=$(CDPATH= cd -- "$(dirname -- "$target")" && pwd)
    target=$(readlink "$target")
    case "$target" in
      /*) ;;
      *) target="$dir/$target" ;;
    esac
  done
  printf '%s\n' "$target"
}

SCRIPT_PATH=$(resolve_script_path "$0")
HERE=$(CDPATH= cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)

case "$(uname -s)" in
  Darwin)
    export DYLD_LIBRARY_PATH="$HERE/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
    ;;
  *)
    export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    ;;
esac

exec "$HERE/bin/rqp.bin" "$@"
EOF
  chmod +x "$bundle_dir/rqp"
}

seen_file_contains() {
  grep -Fqx -- "$1" "$seen_file" 2>/dev/null
}

mark_seen() {
  printf '%s\n' "$1" >> "$seen_file"
}

is_linux_system_library() {
  case "$(basename "$1")" in
    linux-vdso.so.*|ld-linux*.so*|libc.so*|libm.so*|libpthread.so*|libdl.so*|librt.so*|libutil.so*|libgcc_s.so*|libstdc++.so*|libresolv.so*|libcrypt.so*|libnsl.so*|libanl.so*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

collect_linux_dependencies() {
  need_cmd ldd || die 'ldd required for linux release packaging'

  while IFS= read -r line; do
    [ -n "$line" ] || continue
    case "$line" in
      MISSING:*)
        die "missing runtime dependency while packaging release: ${line#MISSING:}"
        ;;
    esac

    dep_path="$line"
    if is_linux_system_library "$dep_path"; then
      continue
    fi

    dep_name="$(basename "$dep_path")"
    if seen_file_contains "$dep_name"; then
      continue
    fi

    copy_following_symlink "$dep_path" "$bundle_dir/lib/$dep_name"
    mark_seen "$dep_name"
  done < <(
    ldd "$bundle_dir/bin/rqp.bin" | awk '
      /=> not found/ { print "MISSING:" $1; next }
      /=>/ && $3 ~ /^\// { print $3; next }
      $1 ~ /^\// { print $1; next }
    '
  )
}

is_macos_system_library() {
  case "$1" in
    /usr/lib/*|/System/Library/*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

rewrite_macos_reference() {
  binary="$1"
  original="$2"
  replacement="$3"
  install_name_tool -change "$original" "$replacement" "$binary"
}

collect_macos_dependencies() {
  need_cmd otool || die 'otool required for macos release packaging'
  need_cmd install_name_tool || die 'install_name_tool required for macos release packaging'

  queue=("$bundle_dir/bin/rqp.bin")
  queue_index=0

  while [ "$queue_index" -lt "${#queue[@]}" ]; do
    current="${queue[$queue_index]}"
    queue_index=$((queue_index + 1))

    while IFS= read -r dep_path; do
      [ -n "$dep_path" ] || continue
      case "$dep_path" in
        @rpath/*|@loader_path/*|@executable_path/*)
          continue
          ;;
      esac
      if is_macos_system_library "$dep_path"; then
        continue
      fi
      [ -e "$dep_path" ] || die "missing runtime dependency while packaging release: $dep_path"

      dep_name="$(basename "$dep_path")"
      bundled_dep="$bundle_dir/lib/$dep_name"
      if ! seen_file_contains "$dep_name"; then
        copy_following_symlink "$dep_path" "$bundled_dep"
        mark_seen "$dep_name"
        queue+=("$bundled_dep")
      fi

      if [ "$current" = "$bundle_dir/bin/rqp.bin" ]; then
        rewrite_macos_reference "$current" "$dep_path" "@executable_path/../lib/$dep_name"
      else
        rewrite_macos_reference "$current" "$dep_path" "@loader_path/$dep_name"
      fi
    done < <(otool -L "$current" | awk 'NR > 1 { print $1 }')

    if [ "$current" != "$bundle_dir/bin/rqp.bin" ]; then
      install_name_tool -id "@loader_path/$(basename "$current")" "$current"
    fi
  done

  if need_cmd codesign; then
    codesign --force --sign - "$bundle_dir/bin/rqp.bin" >/dev/null 2>&1 || true
    for dylib in "$bundle_dir"/lib/*; do
      [ -e "$dylib" ] || continue
      codesign --force --sign - "$dylib" >/dev/null 2>&1 || true
    done
  fi
}

need_cmd tar || die 'tar required for release packaging'
[ -f "$binary_path" ] || die "binary not found: $binary_path"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT INT TERM HUP

bundle_dir="$tmpdir/bundle"
seen_file="$tmpdir/seen.txt"
mkdir -p "$bundle_dir/bin" "$bundle_dir/lib"
: > "$seen_file"

copy_following_symlink "$binary_path" "$bundle_dir/bin/rqp.bin"
chmod +x "$bundle_dir/bin/rqp.bin"
write_launcher

case "$target_family" in
  linux)
    collect_linux_dependencies
    ;;
  macos)
    collect_macos_dependencies
    ;;
  *)
    die "unsupported target family: $target_family"
    ;;
esac

mkdir -p "$(dirname "$output_archive")"
tar -C "$bundle_dir" -czf "$output_archive" .
