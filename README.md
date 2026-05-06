[![CI](https://github.com/Coditary/ReqPack/actions/workflows/ci.yml/badge.svg)](https://github.com/Coditary/ReqPack/actions/workflows/ci.yml)
[![Release](https://github.com/Coditary/ReqPack/actions/workflows/release.yml/badge.svg)](https://github.com/Coditary/ReqPack/actions/workflows/release.yml)
[![Coverage](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/Coditary/ReqPack/main/.github/badges/coverage.json)](https://github.com/Coditary/ReqPack/actions/workflows/coverage.yml)
[![License: 0BSD](https://img.shields.io/badge/license-0BSD-blue.svg)](./LICENSE)

# ReqPack

ReqPack is universal package-manager orchestrator.
It gives you one CLI for installing, updating, auditing, snapshotting, and inspecting packages across multiple ecosystems while keeping real package-manager work inside dedicated plugins.

Examples in this README use `rqp`, because that is built binary name.

## What ReqPack Does Well

- One command surface for multiple ecosystems.
- Manifest-based installs via `reqpack.lua`.
- Plugin wrappers and registry-backed plugin refresh.
- Built-in audit and SBOM export flows.
- Remote command server and remote client profiles.
- XDG-friendly config, cache, and data locations.
- Coverage, test, and release automation in GitHub Actions.

## Quick Start

```bash
rqp install apt curl git
rqp install npm express
rqp install apt:curl npm:express
rqp update --all
rqp list apt
rqp outdated
rqp audit .
rqp sbom --format cyclonedx-json --output sbom.json
rqp snapshot --output reqpack.lua
```

## Requirements

ReqPack core currently targets:

- Linux and macOS.
- CMake 3.15+.
- C++20 compiler.
- Lua 5.4 development files.
- CLI11.
- libcurl.
- Boost.
- OpenSSL.
- fmt.
- spdlog.
- zstd.
- LMDB.
- sol2 headers.
- Git for self-update and Git-backed plugin refresh.

Self-update note:
`rqp update` without package-manager arguments rebuilds ReqPack from configured Git repository.
That means release-binary users still need local build toolchain if they want self-update to work.

## Installation

### Option 1: Use Release Binary

Tagged releases publish archives named `rqp-<tag>-<target>.tar.gz`.
Each archive currently contains `rqp` binary and `SHA256SUMS` is published alongside release assets.

```bash
tar -xzf "rqp-vX.Y.Z-x86_64-linux.tar.gz"
chmod +x rqp
mkdir -p ~/.local/bin
ln -sf "$(pwd)/rqp" ~/.local/bin/rqp
```

Move `rqp` somewhere on your `PATH`, for example `~/.local/bin/rqp`.

### Option 2: Build From Source On Ubuntu Or Debian

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential ca-certificates cmake curl git pkg-config \
  libboost-dev libcli11-dev libcurl4-openssl-dev libfmt-dev \
  liblua5.4-dev libspdlog-dev libssl-dev libzstd-dev

git clone https://github.com/Coditary/ReqPack.git
cd ReqPack
git clone --depth 1 -b v3.3.0 https://github.com/ThePhD/sol2.git /tmp/reqpack-sol2
cmake -S . -B build -DSOL2_INCLUDE_DIR=/tmp/reqpack-sol2/include
cmake --build build --parallel --target ReqPack reqpack_test_targets
ctest --test-dir build --output-on-failure
```

Run binary with:

```bash
./build/rqp --help
```

### Option 3: Build From Source On macOS

```bash
brew install cli11 fmt spdlog boost zstd openssl@3 lua@5.4 ccache

git clone https://github.com/Coditary/ReqPack.git
cd ReqPack
git clone --depth 1 -b v3.3.0 https://github.com/ThePhD/sol2.git /tmp/reqpack-sol2

BREW_PREFIX="$(brew --prefix)"
OPENSSL_PREFIX="$(brew --prefix openssl@3)"
LUA_PREFIX="$(brew --prefix lua@5.4)"
ZSTD_PREFIX="$(brew --prefix zstd)"

cmake -S . -B build \
  -DSOL2_INCLUDE_DIR=/tmp/reqpack-sol2/include \
  -DCMAKE_PREFIX_PATH="${BREW_PREFIX};${OPENSSL_PREFIX};${LUA_PREFIX};${ZSTD_PREFIX}" \
  -DOPENSSL_ROOT_DIR="${OPENSSL_PREFIX}" \
  -DREQPACK_ZSTD_LIBRARY="${ZSTD_PREFIX}/lib/libzstd.dylib" \
  -DLUA_INCLUDE_DIR="${LUA_PREFIX}/include/lua" \
  -DLUA_LIBRARIES="${LUA_PREFIX}/lib/liblua.5.4.dylib"

cmake --build build --parallel --target ReqPack reqpack_test_targets
ctest --test-dir build --output-on-failure
```

## Everyday Usage

### Common Commands

| Command | Purpose | Example |
| --- | --- | --- |
| `install` | Install packages, local targets, or manifest contents | `rqp install apt curl git` |
| `remove` | Remove packages | `rqp remove npm express` |
| `update` | Self-update, refresh plugin wrappers, or update packages | `rqp update --all` |
| `search` | Search available packages | `rqp search apt curl` |
| `list` | List installed packages | `rqp list dnf` |
| `info` | Show package metadata | `rqp info brew jq` |
| `outdated` | Show packages with newer versions | `rqp outdated` |
| `ensure` | Ensure plugin requirements are installed | `rqp ensure apt brew` |
| `audit` | Audit packages or manifests for vulnerabilities | `rqp audit .` |
| `sbom` | Export SBOM for installed packages | `rqp sbom --format cyclonedx-json --output sbom.json` |
| `snapshot` | Write installed-package state to `reqpack.lua` | `rqp snapshot --output reqpack.lua` |
| `host refresh` | Refresh cached host metadata | `rqp host refresh` |
| `serve` | Run stdin or remote command server | `rqp serve --remote --token secret` |
| `remote` | Connect to configured remote profile | `rqp remote dev list apt` |

### Install Packages

```bash
rqp install apt curl git
rqp install npm express lodash brew jq
rqp install apt:curl npm:express
rqp install brew ./my-formula.rb
```

ReqPack also supports manifest installs from current directory or any directory containing `reqpack.lua`:

```bash
rqp install .
rqp install ./myproject
rqp install /absolute/path/to/project
```

Batch mode from stdin is useful for automation:

```bash
printf 'install dnf curl\ninstall npm express\n' | rqp install --stdin
```

### Remove And Update

```bash
rqp remove apt curl
rqp remove apt:curl npm:lodash

rqp update
rqp update --all
rqp update pip
rqp update pip --all
rqp update sys pip
rqp update apt:curl npm:express
```

Update behavior is worth knowing:

- `rqp update` rebuilds ReqPack itself from configured Git repository.
- `rqp update --all` refreshes all known plugin wrappers.
- `rqp update <system>` without package names refreshes that plugin wrapper.
- `rqp update <system> --all` updates all packages for that system.
- `rqp update sys <tool>` updates package-manager binary itself through ReqPack wrapper layer.

### Search, List, Info, Outdated

```bash
rqp search dnf python3 --arch noarch --arch x86_64
rqp list
rqp list apt
rqp info npm express
rqp outdated dnf --type doc
```

`search`, `list`, and `outdated` support repeatable `--arch` and `--type` filters where plugin supports them.

### Safety And Automation Flags

Useful flags you will use often:

- `--dry-run` shows plan without executing it.
- `--non-interactive` disables prompts and uses defaults.
- `--stop-on-first-failure` stops after first failing system.
- `--jobs <n>` forces exact worker count for independent groups.
- `--jobs-max` uses all logical CPU threads.
- `--config <path>` loads custom config Lua file.
- `--registry <path>` loads registry sources from custom path.
- `--archive-password <value>` sets password for encrypted archives.

Archive-password note:
you can also provide archive password through `REQPACK_ARCHIVE_PASSWORD`.

### Snapshot And Restore

`snapshot` writes packages tracked in ReqPack history into portable `reqpack.lua` manifest.

```bash
rqp snapshot --output reqpack.lua
rqp install .
```

### Audit

ReqPack can audit installed packages, explicit package lists, or manifests.

```bash
rqp audit
rqp audit npm react
rqp audit npm:react maven:org.junit:junit
rqp audit .
rqp audit ./reqpack.lua
rqp audit --format cyclonedx-vex-json --output audit.json
rqp audit --format sarif --output audit.sarif
```

Audit behavior:

- Without `--output`, exit code is `1` when findings exist.
- With `--output`, export still succeeds with exit code `0`.
- Output formats: `table`, `json`, `cyclonedx-vex-json`, `sarif`.

### SBOM

ReqPack can export installed-package inventory as terminal table, JSON, or CycloneDX JSON.

```bash
rqp sbom
rqp sbom apt npm
rqp sbom --format json
rqp sbom --format cyclonedx-json --output sbom.json
```

SBOM output formats:

- `table`
- `json`
- `cyclonedx-json`

Useful SBOM flags:

- `--wide`
- `--no-wrap`
- `--force`
- `--sbom-skip-missing-packages`

### Remote Mode

ReqPack has two remote pieces:

- `rqp serve --remote` starts remote TCP command server.
- `rqp remote <profile>` connects to profile from `remote.lua`.

Today, text and JSON protocols are usable.
`--http` and `--https` are reserved for future server mode and should not be documented as production-ready yet.

Minimal `~/.config/reqpack/remote.lua` example:

```lua
profiles = {
  dev = {
    host = "127.0.0.1",
    port = 4545,
    protocol = "auto",
    token = "secret",
  },
}

users = {
  admin = {
    token = "secret",
    isAdmin = true,
  },
}
```

Examples:

```bash
rqp serve --remote --bind 127.0.0.1 --port 4545 --token secret
rqp serve --remote --json --readonly --max-connections 4
rqp remote dev list apt
rqp remote dev install apt curl
```

## Manifest Example

Project manifests use `reqpack.lua`.

```lua
return {
  packages = {
    { system = "dnf", name = "curl" },
    { system = "npm", name = "express", version = "4.18.0" },
  }
}
```

## Minimal Config Example

Minimal `~/.config/reqpack/config.lua` example:

```lua
return {
  interaction = {
    interactive = true,
  },
  execution = {
    jobs = 4,
    jobsMode = "fixed",
  },
  security = {
    onUnsafe = "prompt",
    severityThreshold = "critical",
  },
  registry = {
    remoteUrl = "https://github.com/Coditary/rqp-registry.git",
  },
  selfUpdate = {
    branch = "main",
    linkPath = "~/.local/bin/rqp",
  },
}
```

Useful config areas for daily use:

- `interaction.interactive`
- `execution.jobs` and `execution.jobsMode`
- `security.onUnsafe`, `security.severityThreshold`, `security.scoreThreshold`
- `registry.remoteUrl`, `registry.pluginDirectory`, `registry.sources`
- `selfUpdate.repoUrl`, `selfUpdate.branch`, `selfUpdate.linkPath`
- `sbom.defaultFormat`, `sbom.defaultOutputPath`

## Config And File Locations

ReqPack follows XDG base-directory rules.

Config:

- `$XDG_CONFIG_HOME/reqpack/config.lua`
- `$XDG_CONFIG_HOME/reqpack/remote.lua`
- fallback: `~/.config/reqpack/...`

Data:

- `$XDG_DATA_HOME/reqpack/plugins`
- `$XDG_DATA_HOME/reqpack/repos`
- `$XDG_DATA_HOME/reqpack/registry`
- `$XDG_DATA_HOME/reqpack/history`
- `$XDG_DATA_HOME/reqpack/rqp/state`
- `$XDG_DATA_HOME/reqpack/self/repo`
- `$XDG_DATA_HOME/reqpack/self/build`
- `$XDG_DATA_HOME/reqpack/self/bin`
- `$XDG_DATA_HOME/reqpack/security/index`
- `$XDG_DATA_HOME/reqpack/security/osv`
- fallback: `~/.local/share/reqpack/...`

Cache:

- `$XDG_CACHE_HOME/reqpack/transactions`
- `$XDG_CACHE_HOME/reqpack/security/cache`
- `$XDG_CACHE_HOME/reqpack/host/info.v1.json`
- fallback: `~/.cache/reqpack/...`

Binary link:

- self-update symlink default: `~/.local/bin/rqp`

Registry notes:

- default registry remote: `https://github.com/Coditary/rqp-registry.git`
- local workspace `plugins/` directory is auto-used when you run `rqp` from repo root and no custom plugin directory is configured

Bundled or checked-in plugin examples in this repository:

- `plugins/dnf`
- `plugins/maven`
- `plugins/java`
- `plugins/sys`

## Development Shortcuts

```bash
make build
make test
make test-unit
make test-smoke
make test-coverage
make profile-tests
```

Coverage writes `Coverage.xml` below `build/coverage/Testing/...` and summary is derived from `src/main/cpp` sources.
Profiling writes reports below `build/profile/profile-data/`.

## CI/CD

Repo ships three documentation-visible automation signals:

- `CI`: build and test matrix for `x86_64-linux`, `aarch64-linux`, and `aarch64-darwin`.
- `Release`: tag-driven packaging flow for `x86_64-linux`, `aarch64-linux`, `x86_64-darwin`, and `aarch64-darwin` plus `SHA256SUMS` publication.
- `Coverage`: Linux coverage run that updates README badge from GitHub Actions on pushes to `main` and uploads report artifacts for pull requests.

Coverage badge note:
it will show `pending` until first successful `Coverage` workflow run on `main` writes `.github/badges/coverage.json`.

Relevant workflow files:

- `.github/workflows/ci.yml`
- `.github/workflows/release.yml`
- `.github/workflows/coverage.yml`

## Repository Layout

- `src/main/cpp`: core implementation.
- `src/main/include`: public and internal headers.
- `plugins/`: built-in or locally checked-in plugins.
- `tests/`: unit, integration, coverage, and profiling helpers.
- `.github/workflows/`: CI, coverage, and release automation.

## Contributing

Contribution guide lives in [`CONTRIBUTING.md`](./CONTRIBUTING.md).
If you want to improve core behavior, add tests, report bugs, or propose plugin/runtime changes, start there.

## License

ReqPack is licensed under `0BSD`.
Use it, modify it, fork it, ship it.
No attribution requirements, no field-of-use restrictions, no warranty.
