# Contributing To ReqPack

Thanks for improving ReqPack.
This guide is written for people touching core C++ code, workflows, documentation, plugins, and issue triage.

## Good First Contributions

- Fix incorrect CLI behavior.
- Add or tighten unit and integration tests.
- Improve plugin error messages.
- Improve README, help text, and examples.
- Add missing documentation around registry, remote mode, or manifests.
- Reduce platform-specific build friction.

## Before You Start

- Check existing issues and pull requests first.
- For larger behavior changes, open issue or discussion before writing a big patch.
- Keep patches focused. Small, reviewable pull requests move fastest.
- If you change user-facing behavior, update docs and CLI help in same pull request.

## Development Setup

### Ubuntu Or Debian

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

### macOS

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

## Daily Dev Commands

```bash
make build
make test
make test-unit
make test-smoke
make test-coverage
make profile-tests
```

What they do:

- `make test`: full local test pass.
- `make test-unit`: unit-only pass.
- `make test-smoke`: integration-focused pass.
- `make test-coverage`: build with coverage instrumentation, run tests, emit coverage summary.
- `make profile-tests`: build with profiling flags and summarize hotspots.

## Core Map

Main places you will touch most:

- `src/main/cpp/cli`: CLI parsing and command help text.
- `src/main/cpp/core`: orchestration, registry, downloader, executor, audit, SBOM, remote, history.
- `src/main/cpp/output`: terminal rendering and logger behavior.
- `src/main/include`: interfaces and shared types.
- `tests/unit`: narrow behavioral coverage.
- `tests/integration`: cross-component behavior.
- `plugins/`: built-in plugin wrappers and plugin-specific docs.

## Guidelines For Core Changes

- Prefer smallest correct change.
- Preserve existing command semantics unless change is intentional and documented.
- Add or update tests when behavior changes.
- Keep CLI help text in sync with parser behavior.
- Keep README examples in sync with real commands.
- If change touches workflows, release packaging, or coverage reporting, mention that clearly in pull request summary.

Strong recommendation:
if you modify command syntax or defaults, update these three places together:

- implementation
- tests
- `README.md` and command help in `src/main/cpp/cli/cli.cpp`

## Reporting Issues

Good bug reports are reproducible.
Please include:

- operating system and architecture
- how ReqPack was installed
- exact command you ran
- expected behavior
- actual behavior
- full error output
- relevant config or manifest snippet
- whether problem reproduces with `--non-interactive` or `--dry-run`

If issue depends on specific plugin or package manager, mention that explicitly.

## Feature Requests

Feature requests are most useful when they explain:

- problem you are trying to solve
- why current behavior is insufficient
- expected CLI shape or workflow
- compatibility concerns
- whether change affects plugins, registry, remote mode, or output format

## Pull Requests

Pull request checklist:

- patch is focused and reviewable
- tests added or updated where needed
- docs/help text updated for user-facing changes
- build passes locally when practical
- no unrelated formatting churn
- no generated build artifacts committed

Good pull request descriptions include:

- what changed
- why it changed
- risk or compatibility notes
- how it was tested

## Plugins And Registry Changes

If contribution adds or changes plugin behavior:

- keep plugin-specific docs close to plugin directory
- document required external binaries or host dependencies
- explain bootstrap behavior clearly
- test both happy path and common failure path when possible

If contribution changes registry handling:

- mention migration or compatibility impact
- document any new config keys or trust assumptions

## Security Notes

Do not post secrets, tokens, passwords, or private registry credentials in issues or pull requests.
If report contains sensitive vulnerability details, use private disclosure channel if available instead of public issue.

## License

By contributing, you agree your contributions are released under project `0BSD` license.
