#!/usr/bin/env bash
# Build the linux release bundle inside a distro container (Bookworm or Fedora) for glibc-matched live tests.
set -euo pipefail

if [ "$#" -lt 3 ]; then
    printf 'usage: %s <podman|docker> <monorepo-root> <work-root> [reqpack-source-dir]\n' "$0" >&2
    exit 1
fi

engine="$1"
monorepo_root="$2"
work_root="$3"
reqpack_dir="${4:-${monorepo_root}/reqpack}"

if [ ! -f "${reqpack_dir}/CMakeLists.txt" ]; then
    printf 'reqpack source not found: %s\n' "${reqpack_dir}" >&2
    exit 1
fi

mkdir -p "${work_root}"

stage_root="${work_root}/monorepo-stage"
mkdir -p "${stage_root}"
for component in reqpack ReqPack-Core reqpack-security-core; do
    if [ -d "${monorepo_root}/${component}" ]; then
        rsync -a --delete \
            --exclude='build/' --exclude='.git/' --exclude='fuzz/' \
            "${monorepo_root}/${component}/" "${stage_root}/${component}/"
    fi
done

mount_monorepo="${stage_root}:/monorepo:ro"
mount_work="${work_root}:/work:rw"

bundle_image="${REQPACK_LIVE_BUNDLE_IMAGE:-debian:bookworm-slim}"
bundle_family=debian
case "${bundle_image}" in
  fedora*) bundle_family=fedora ;;
esac

completion_dir=""
if [ -n "${REQPACK_COMPLETION_DIR:-}" ] && [ -f "${REQPACK_COMPLETION_DIR}/CMakeLists.txt" ]; then
    completion_dir="${REQPACK_COMPLETION_DIR}"
else
    for candidate in \
        "${HOME}/Dev/Coditary/shared/Shared-Cpp/completion" \
        "${monorepo_root}/../shared/Shared-Cpp/completion" \
        "${monorepo_root}/../../Coditary/shared/Shared-Cpp/completion"; do
        if [ -f "${candidate}/CMakeLists.txt" ]; then
            completion_dir="${candidate}"
            break
        fi
    done
fi

if [ -n "${completion_dir}" ]; then
    mkdir -p "${stage_root}/Shared-Cpp/completion"
    rsync -a --delete "${completion_dir}/" "${stage_root}/Shared-Cpp/completion/"
else
    printf 'Shared-Cpp completion not found; set REQPACK_COMPLETION_DIR or clone Coditary/shared\n' >&2
    exit 1
fi

if [ "${bundle_family}" = fedora ]; then
    inner=$(cat <<'INNER'
set -euo pipefail
dnf install -y \
  bash ca-certificates cmake curl git gcc-c++ pkgconfig \
  boost-devel libcurl-devel fmt-devel spdlog-devel libzstd-devel openssl-devel \
  libffi-devel lua-devel cli11-devel
GO_VERSION="${REQPACK_GO_VERSION:-1.23.6}"
go_arch=amd64
case "$(uname -m)" in
  aarch64|arm64) go_arch=arm64 ;;
esac
curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-${go_arch}.tar.gz" -o /tmp/go.tgz
rm -rf /usr/local/go
tar -C /usr/local -xzf /tmp/go.tgz
export PATH="/usr/local/go/bin:${PATH}"
REQPACK_SRC="/monorepo/reqpack"
if [ ! -f "${REQPACK_SRC}/CMakeLists.txt" ] && [ -f "/monorepo/CMakeLists.txt" ]; then
  REQPACK_SRC="/monorepo"
fi
SOL2_DIR="/work/third_party/include"
bash "${REQPACK_SRC}/.github/scripts/setup-sol2.sh" "${SOL2_DIR}"
rm -rf /work/build
cmake_args=(-S "${REQPACK_SRC}" -B /work/build -DCMAKE_BUILD_TYPE=Release \
  -DSOL2_INCLUDE_DIR="${SOL2_DIR}" -DREQPACK_GO_EXECUTABLE=/usr/local/go/bin/go)
if [ -f /monorepo/Shared-Cpp/completion/CMakeLists.txt ]; then
  cmake_args+=(-DCOMPLETION_DIR=/monorepo/Shared-Cpp/completion)
fi
cmake "${cmake_args[@]}"
cmake --build /work/build --parallel "$(nproc)" --target ReqPack
bash "${REQPACK_SRC}/scripts/package_release_bundle.sh" \
  /work/build/rqp /work/rqp-linux.tar.gz linux
INNER
)
else
    inner=$(cat <<INNER
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential ca-certificates cmake curl git pkg-config \
  libboost-dev libcurl4-openssl-dev libfmt-dev \
  libffi-dev liblua5.4-dev libspdlog-dev libssl-dev libzstd-dev
apt-get purge -y libcli11-dev 2>/dev/null || true

GO_VERSION="\${REQPACK_GO_VERSION:-1.23.6}"
go_arch=amd64
case "\$(uname -m)" in
  aarch64|arm64) go_arch=arm64 ;;
esac
curl -fsSL "https://go.dev/dl/go\${GO_VERSION}.linux-\${go_arch}.tar.gz" -o /tmp/go.tgz
rm -rf /usr/local/go
tar -C /usr/local -xzf /tmp/go.tgz
export PATH="/usr/local/go/bin:\${PATH}"

REQPACK_SRC="/monorepo/reqpack"
if [ ! -f "\${REQPACK_SRC}/CMakeLists.txt" ] && [ -f "/monorepo/CMakeLists.txt" ]; then
  REQPACK_SRC="/monorepo"
fi

SOL2_DIR="/work/third_party/include"
bash "\${REQPACK_SRC}/.github/scripts/setup-sol2.sh" "\${SOL2_DIR}"

CLI11_PREFIX="/work/deps"
git clone --depth 1 --branch v2.7.2 https://github.com/CLIUtils/CLI11.git /work/third_party/CLI11
cmake -S /work/third_party/CLI11 -B /work/build-cli11 -DCMAKE_BUILD_TYPE=Release \
  -DCLI11_BUILD_TESTS=OFF -DCLI11_BUILD_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="\${CLI11_PREFIX}"
cmake --build /work/build-cli11 --parallel "\$(nproc)"
cmake --install /work/build-cli11

rm -rf /work/build

cmake_args=(-S "\${REQPACK_SRC}" -B /work/build -DCMAKE_BUILD_TYPE=Release \
  -DSOL2_INCLUDE_DIR="\${SOL2_DIR}" -DREQPACK_GO_EXECUTABLE=/usr/local/go/bin/go \
  -DCMAKE_PREFIX_PATH="\${CLI11_PREFIX}" -DCLI11_DIR="\${CLI11_PREFIX}/share/cmake/CLI11" \
  -DCMAKE_CXX_FLAGS="-I\${CLI11_PREFIX}/include")
if [ -f /monorepo/Shared-Cpp/completion/CMakeLists.txt ]; then
  cmake_args+=(-DCOMPLETION_DIR=/monorepo/Shared-Cpp/completion)
fi
cmake "\${cmake_args[@]}"
cmake --build /work/build --parallel "\$(nproc)" --target ReqPack
bash "\${REQPACK_SRC}/scripts/package_release_bundle.sh" \
  /work/build/rqp /work/rqp-linux.tar.gz linux
INNER
)
fi

"${engine}" run --rm --security-opt label=disable \
  -v "${mount_monorepo}" \
  -v "${mount_work}" \
  "${bundle_image}" \
  bash -lc "${inner}"

if [ ! -f "${work_root}/rqp-linux.tar.gz" ]; then
    printf 'bundle archive missing after container build\n' >&2
    exit 1
fi
