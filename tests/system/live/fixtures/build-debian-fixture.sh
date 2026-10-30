#!/usr/bin/env bash
set -euo pipefail

root="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
pkg="${root}/.build/pkg"
out="${root}/debian/reqpack-live-fixture_1.0_all.deb"

rm -rf "${pkg}"
mkdir -p "${pkg}/DEBIAN" "${pkg}/usr/bin"

cat >"${pkg}/DEBIAN/control" <<'EOF'
Package: reqpack-live-fixture
Version: 1.0
Section: misc
Priority: optional
Architecture: all
Maintainer: ReqPack Tests <tests@coditary.com>
Description: Tiny package for offline live container tests
EOF

cat >"${pkg}/usr/bin/rqplive-fixture" <<'EOF'
#!/bin/sh
printf 'reqpack-live-fixture\n'
EOF
chmod 755 "${pkg}/usr/bin/rqplive-fixture"

mkdir -p "${root}/debian"
dpkg-deb --build "${pkg}" "${out}"
printf 'built %s\n' "${out}"
