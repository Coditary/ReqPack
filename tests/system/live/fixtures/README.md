# Local package fixtures (optional)

Offline packages for live matrix rows (`mode=local` and `plugin-live`):

- `debian/reqpack-live-fixture_1.0_all.deb`
- `rpm/reqpack-live-fixture-1.0-1.noarch.rpm`
- `arch/reqpack-live-fixture-1.0-1-any.pkg.tar.zst`
- `npm/reqpack-live-fixture-1.0.0.tgz`

Rebuild everything:

```bash
bash build-all-fixtures.sh
```

Uses Podman or Docker (`CONTAINER_BIN`). Debian, rpm, and npm try fast host tools first (`dpkg-deb`, `rpmbuild`, `npm pack`) and fall back to distro containers when missing or on failure. Pacman always builds inside Arch (makepkg cannot run as root). Force containers only: `LIVE_FIXTURES_FORCE_CONTAINER=1`.

GHCR cached base images: workflow `.github/workflows/publish-live-test-images.yml` publishes
`ghcr.io/coditary/reqpack-live-test:<distro>`. Set `LIVE_IMAGE_REGISTRY=ghcr.io/coditary` when running
live tests to pull instead of rebuilding locally.
