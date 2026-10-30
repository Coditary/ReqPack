# Live container plugin tests

Third test tier beside CTest mocks and hermetic `rqp test-plugin` cases.

These tests mount a real Linux `rqp` bundle and selected plugins from `reqpack-plugins`
into official distro base images, then run install/search/list/info/outdated/remove
against the real package manager inside the container.

## Prerequisites

- Podman or Docker on the host
- `reqpack-plugins` checkout next to `reqpack` (or set `REQPACK_PLUGINS_ROOT`)

## Commands

```bash
# Full matrix (builds rqp + cached distro images)
make system-tests-live

# Fast smoke (Debian dpkg only)
make system-tests-live-quick

# Parallel matrix rows
make system-tests-live-parallel LIVE_SYSTEM_TEST_JOBS=4

# CTest label
ctest --test-dir build -L container-live --output-on-failure
```

## Matrix

Edit `matrix.tsv` to add rows (`id|image|system|plugin_relpath|package|verify_cmd|enabled`).

Cached base images are defined under `images/*/Containerfile` (index sync at image build time).
Publish to GHCR: `LIVE_IMAGE_PUSH=1 bash scripts/publish-live-test-images.sh` (see workflow `publish-live-test-images.yml`).

Rebuild all offline artifacts (Podman/Docker required for pacman; debian/rpm/npm fall back to containers if host tools are missing):

```bash
bash tests/system/live/fixtures/build-all-fixtures.sh
# force containers only: LIVE_FIXTURES_FORCE_CONTAINER=1 bash ...
```

Matrix column `mode`:

- `repo` — full lifecycle against distro mirrors (`run_live_lifecycle.sh`)
- `local` — offline install from `fixtures/` (`package` column: `path/to.art:remove-name`)
- `plugin-live` — `rqp test-plugin --live` using `.reqpack-test/live/` cases (`run_live_plugin_test.sh`)

Parallel runs register containers as `rqp-live-<run-id>-<row>` and remove them on exit/interrupt.

## Container runtime

Shared helpers live in `tests/system/lib/container_runtime.sh` (auto-detect Podman/Docker,
SELinux volume suffixes). Hermetic system tests use the same layer.
