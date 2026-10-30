#!/usr/bin/env bash
# Builds offline live-test packages. Host tools are optional; missing tools fall back to
# Podman/Docker distro containers (works on macOS and minimal Linux installs).
set -euo pipefail

root="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
mkdir -p "${root}/debian" "${root}/rpm" "${root}/arch" "${root}/npm"

CONTAINER_BIN="${CONTAINER_BIN:-}"
if [ -z "${CONTAINER_BIN}" ]; then
    if command -v podman >/dev/null 2>&1; then
        CONTAINER_BIN=podman
    elif command -v docker >/dev/null 2>&1; then
        CONTAINER_BIN=docker
    else
        echo "Error: set CONTAINER_BIN or install podman/docker to build fixtures." >&2
        exit 1
    fi
fi

container_run() {
    "${CONTAINER_BIN}" run --rm "$@"
}

build_debian_container() {
    container_run debian:bookworm-slim bash -lc '
        set -euo pipefail
        apt-get update -qq
        apt-get install -y -qq dpkg-dev >/dev/null
        pkg=/tmp/pkg
        mkdir -p "$pkg/DEBIAN" "$pkg/usr/bin"
        printf "Package: reqpack-live-fixture\nVersion: 1.0\nArchitecture: all\nMaintainer: test\nDescription: tiny\n" > "$pkg/DEBIAN/control"
        printf "#!/bin/sh\necho ok\n" > "$pkg/usr/bin/rqplive-fixture"
        chmod 755 "$pkg/usr/bin/rqplive-fixture"
        dpkg-deb --build "$pkg" /tmp/fix.deb >/dev/null
        cat /tmp/fix.deb
    ' >"${root}/debian/reqpack-live-fixture_1.0_all.deb"
}

build_debian_host() {
    local out="${root}/debian/reqpack-live-fixture_1.0_all.deb"
    local pkg
    pkg="$(mktemp -d)"
    mkdir -p "${pkg}/DEBIAN" "${pkg}/usr/bin"
    printf "Package: reqpack-live-fixture\nVersion: 1.0\nArchitecture: all\nMaintainer: test\nDescription: tiny\n" >"${pkg}/DEBIAN/control"
    printf "#!/bin/sh\necho ok\n" >"${pkg}/usr/bin/rqplive-fixture"
    chmod 755 "${pkg}/usr/bin/rqplive-fixture"
    dpkg-deb --build "${pkg}" "${out}" >/dev/null
    rm -rf "${pkg}"
}

build_debian() {
    if [ "${LIVE_FIXTURES_FORCE_CONTAINER:-0}" != "1" ] && command -v dpkg-deb >/dev/null 2>&1; then
        printf '[fixtures] debian: host (dpkg-deb)\n' >&2
        build_debian_host
        return
    fi
    printf '[fixtures] debian: container\n' >&2
    build_debian_container
}

build_rpm_container() {
    container_run fedora:43 bash -lc '
        set -euo pipefail
        dnf install -y -q rpm-build >/dev/null
        work=/tmp/rpm
        mkdir -p "$work"/{BUILD,RPMS,SOURCES,SPECS,SRPMS,BUILDROOT}
        cat > "$work/SPECS/reqpack-live-fixture.spec" <<EOF
Name:           reqpack-live-fixture
Version:        1.0
Release:        1%{?dist}
Summary:        Tiny live test package
License:        MIT
BuildArch:      noarch
%description
Tiny live test package
%install
mkdir -p %{buildroot}/usr/bin
echo '"'"'#!/bin/sh'"'"' > %{buildroot}/usr/bin/rqplive-fixture
chmod 755 %{buildroot}/usr/bin/rqplive-fixture
%files
/usr/bin/rqplive-fixture
EOF
        rpmbuild --define "_topdir $work" -bb "$work/SPECS/reqpack-live-fixture.spec" >/dev/null 2>&1
        cat "$work/RPMS/noarch/"*.rpm
    ' >"${root}/rpm/reqpack-live-fixture-1.0-1.noarch.rpm"
}

build_rpm_host() {
    local out="${root}/rpm/reqpack-live-fixture-1.0-1.noarch.rpm"
    local work
    work="$(mktemp -d)"
    mkdir -p "${work}"/{BUILD,RPMS,SOURCES,SPECS,SRPMS,BUILDROOT}
    cat >"${work}/SPECS/reqpack-live-fixture.spec" <<'EOF'
Name:           reqpack-live-fixture
Version:        1.0
Release:        1%{?dist}
Summary:        Tiny live test package
License:        MIT
BuildArch:      noarch
%description
Tiny live test package
%install
mkdir -p %{buildroot}/usr/bin
echo '#!/bin/sh' > %{buildroot}/usr/bin/rqplive-fixture
chmod 755 %{buildroot}/usr/bin/rqplive-fixture
%files
/usr/bin/rqplive-fixture
EOF
    rpmbuild --define "_topdir ${work}" -bb "${work}/SPECS/reqpack-live-fixture.spec" >/dev/null 2>&1
    cp "${work}"/RPMS/noarch/*.rpm "${out}"
    rm -rf "${work}"
}

build_rpm() {
    if [ "${LIVE_FIXTURES_FORCE_CONTAINER:-0}" != "1" ] && command -v rpmbuild >/dev/null 2>&1; then
        printf '[fixtures] rpm: host (rpmbuild)\n' >&2
        build_rpm_host || {
            printf '[fixtures] rpm: host failed, using container\n' >&2
            build_rpm_container
        }
        return
    fi
    printf '[fixtures] rpm: container\n' >&2
    build_rpm_container
}

build_pacman_container() {
    container_run archlinux:latest bash -lc '
        set -euo pipefail
        pacman -Sy --noconfirm base-devel >/dev/null
        useradd -m builduser
        work=/home/builduser/pkg
        mkdir -p "$work"
        chown builduser:builduser "$work"
        su - builduser -c "
          set -euo pipefail
          cd \"$work\"
          printf '"'"'#!/bin/sh\necho ok\n'"'"' > rqplive-fixture
          chmod 755 rqplive-fixture
          cat > PKGBUILD <<'"'"'EOF'"'"'
pkgname=reqpack-live-fixture
pkgver=1.0
pkgrel=1
arch=(any)
package() {
  install -Dm755 \"\$startdir/rqplive-fixture\" \"\$pkgdir/usr/bin/rqplive-fixture\"
}
EOF
          makepkg --noconfirm --skipinteg --skipchecksums >/dev/null
          cat *.pkg.tar.*
        "
    ' >"${root}/arch/reqpack-live-fixture-1.0-1-any.pkg.tar.zst"
}

build_pacman() {
    printf '[fixtures] pacman: container (makepkg)\n' >&2
    build_pacman_container
}

build_npm_container() {
    container_run node:22-bookworm-slim bash -lc '
        set -euo pipefail
        work=/tmp/npm
        mkdir -p "$work/pkg"
        cd "$work/pkg"
        cat > package.json <<EOF
{
  "name": "reqpack-live-fixture",
  "version": "1.0.0",
  "description": "tiny live test package",
  "bin": { "rqplive-fixture": "cli.js" }
}
EOF
        printf "#!/usr/bin/env node\nconsole.log('"'"'reqpack-live-fixture'"'"');\n" > cli.js
        chmod 755 cli.js
        npm pack --silent >/dev/null
        cat reqpack-live-fixture-1.0.0.tgz
    ' >"${root}/npm/reqpack-live-fixture-1.0.0.tgz"
}

build_npm_host() {
    local out="${root}/npm/reqpack-live-fixture-1.0.0.tgz"
    local work
    work="$(mktemp -d)"
    cd "${work}"
    cat >package.json <<'EOF'
{
  "name": "reqpack-live-fixture",
  "version": "1.0.0",
  "description": "tiny live test package",
  "bin": { "rqplive-fixture": "cli.js" }
}
EOF
    printf '#!/usr/bin/env node\nconsole.log("reqpack-live-fixture");\n' >cli.js
    chmod 755 cli.js
    npm pack --silent >/dev/null
    cp reqpack-live-fixture-1.0.0.tgz "${out}"
    rm -rf "${work}"
}

build_npm() {
    if [ "${LIVE_FIXTURES_FORCE_CONTAINER:-0}" != "1" ] && command -v npm >/dev/null 2>&1; then
        printf '[fixtures] npm: host (npm pack)\n' >&2
        build_npm_host || {
            printf '[fixtures] npm: host failed, using container\n' >&2
            build_npm_container
        }
        return
    fi
    printf '[fixtures] npm: container\n' >&2
    build_npm_container
}

printf '[fixtures] engine: %s\n' "${CONTAINER_BIN}" >&2
build_debian
build_rpm
build_pacman
build_npm

printf 'fixtures written under %s\n' "${root}"
