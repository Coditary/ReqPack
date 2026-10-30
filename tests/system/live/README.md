# Live container plugin tests

Third test tier beside CTest mocks and hermetic `rqp test-plugin` cases.

Orchestration is implemented in **`rqp live run`** (C++). `scripts/run-live-system-tests.sh` is a thin wrapper that execs the built `rqp` binary.

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

**Nix:** `images/nix` preinstalls the distro `nix` package (fast conformance on an already-working PM). **`images/nix-bootstrap`** has **no** `nix` package — rows `*-bootstrap-*` exercise plugin bootstrap via `context.net.download` and must pass `P1.5`/`P1.6` and real `rqp search` (no `conf_nix_hello_resolvable` bypass).

Publish to GHCR: `LIVE_IMAGE_PUSH=1 bash scripts/publish-live-test-images.sh` (see workflow `publish-live-test-images.yml`).

Rebuild all offline artifacts (Podman/Docker required for pacman; debian/rpm/npm fall back to containers if host tools are missing):

```bash
bash tests/system/live/fixtures/build-all-fixtures.sh
# force containers only: LIVE_FIXTURES_FORCE_CONTAINER=1 bash ...
```

Matrix column `mode`:

- `repo` — full lifecycle against distro mirrors (`run_live_lifecycle.sh`)
- `local` — offline install from `fixtures/` (`package` column: `path/to.art:remove-name`)
- `plugin-live` — pack `.rqp` on the host, `rqp install rqp` in the container, then `rqp test-plugin --live`
- `conformance` — **Plugin Conformance Suite**: same `.rqp` bootstrap as `plugin-live`, then standard
  `.reqpack-test/live/` cases (Ebene A) and `run_live_conformance.sh` with `profiles/<system>.env` (Ebene B, 7 phases)

Parametric profiles live under `profiles/*.env` (offline fixture paths, remote smoke package, arch tokens).
Rebuild offline v1/v2 fixtures: `bash tests/system/live/fixtures/build-all-fixtures.sh`.

Parallel runs register containers as `rqp-live-<run-id>-<row>` and remove them on exit/interrupt.

### Ephemeral work directories

Each `rqp live run` without `--work-root` creates `build/live-system-tests/run-<timestamp>-<pid>/`
(bundle extract, row workspaces, reports). That directory is **removed automatically** when the run
finishes (pass or fail). Use `--keep-work` or `REQPACK_LIVE_KEEP_WORK=1` to retain it for debugging.
Explicit `--work-root` paths are never deleted. `--prepare-only` keeps the prepared tree.

Offline package fixtures under `fixtures/debian/` etc. are **not** removed by live runs: they are
rebuilt only when you run `fixtures/build-all-fixtures.sh` (some are committed for CI).

## Container runtime

Shared helpers live in `tests/system/lib/container_runtime.sh` (auto-detect Podman/Docker,
SELinux volume suffixes). Hermetic system tests use the same layer.
