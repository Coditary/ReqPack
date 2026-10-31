#!/usr/bin/env bash
# Build the portable Linux release tarball inside debian:bookworm so the binary
# runs on glibc 2.36+ (Bookworm, RHEL 9-era, etc.), not only Ubuntu 24.04+.
set -euo pipefail

if [ "$#" -ne 2 ]; then
    printf 'usage: %s <repo-root> <output-archive>\n' "$0" >&2
    exit 1
fi

repo_root="$1"
output_archive="$2"
release_id="${RELEASE_ID:-dev}"
image="${REQPACK_LINUX_BUILD_IMAGE:-debian:bookworm}"

repo_root="$(CDPATH= cd -- "$repo_root" && pwd)"
output_archive="$(CDPATH= cd -- "$(dirname "$output_archive")" && pwd)/$(basename "$output_archive")"
mkdir -p "$(dirname "$output_archive")"

engine="${CONTAINER_ENGINE:-}"
if [ -z "$engine" ]; then
    if command -v docker >/dev/null 2>&1; then
        engine=docker
    elif command -v podman >/dev/null 2>&1; then
        engine=podman
    else
        printf 'Need docker or podman to build linux release bundle\n' >&2
        exit 1
    fi
fi

out_dir="$(dirname "$output_archive")"
out_name="$(basename "$output_archive")"

completion_dir="${REQPACK_COMPLETION_DIR:-}"
if [ -z "$completion_dir" ]; then
    for candidate in \
        "${HOME}/Dev/Coditary/shared/Shared-Cpp/completion" \
        "${repo_root}/../../Coditary/shared/Shared-Cpp/completion" \
        "${repo_root}/../Coditary/shared/Shared-Cpp/completion"; do
        if [ -f "${candidate}/CMakeLists.txt" ]; then
            completion_dir="${candidate}"
            break
        fi
    done
fi

extra_mount=()
cmake_completion_arg=""
if [ -n "$completion_dir" ] && [ -f "${completion_dir}/CMakeLists.txt" ]; then
    completion_dir="$(CDPATH= cd -- "$completion_dir" && pwd)"
    extra_mount=(-v "${completion_dir}:/completion:ro")
    cmake_completion_arg="-DCOMPLETION_DIR=/completion"
else
    printf 'Shared-Cpp completion not found; set REQPACK_COMPLETION_DIR or clone Coditary/shared\n' >&2
    exit 1
fi

"${engine}" run --rm \
    --security-opt label=disable \
    -v "${repo_root}:/src:rw" \
    -v "${out_dir}:/out:rw" \
    "${extra_mount[@]}" \
    -e "RELEASE_ID=${release_id}" \
    -e "OUT_NAME=${out_name}" \
    -e "GO_VERSION=${GO_VERSION:-1.23.6}" \
    -e "SOL2_VERSION=${SOL2_VERSION:-v3.3.0}" \
    -e "CMAKE_COMPLETION_ARG=${cmake_completion_arg}" \
    "${image}" \
    bash -lc '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake curl git pkg-config \
    libboost-dev libcli11-dev libcurl4-openssl-dev libfmt-dev \
    libffi-dev liblua5.4-dev libspdlog-dev libssl-dev libzstd-dev

go_arch=amd64
case "$(uname -m)" in
    aarch64|arm64) go_arch=arm64 ;;
esac
curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-${go_arch}.tar.gz" -o /tmp/go.tgz
rm -rf /usr/local/go
tar -C /usr/local -xzf /tmp/go.tgz
export PATH="/usr/local/go/bin:${PATH}"

cd /src
rm -rf build
mkdir -p third_party/include
bash .github/scripts/setup-sol2.sh third_party/include

bash .github/scripts/configure-build.sh build linux \
    -DSOL2_INCLUDE_DIR="/src/third_party/include" \
    -DREQPACK_GO_EXECUTABLE=/usr/local/go/bin/go \
    ${CMAKE_COMPLETION_ARG}
cmake --build build --parallel 1 --target ReqPack

bundle_path="/out/${OUT_NAME}"
bash scripts/package_release_bundle.sh build/rqp "${bundle_path}" linux

tmpdir=$(mktemp -d)
trap "rm -rf \"${tmpdir}\"" EXIT INT TERM HUP
tar -xzf "${bundle_path}" -C "${tmpdir}"
"${tmpdir}/rqp" --help >/dev/null
'

printf 'Built linux release bundle: %s\n' "$output_archive"
