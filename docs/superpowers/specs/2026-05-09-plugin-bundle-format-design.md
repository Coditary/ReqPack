# Plugin Bundle Format Design

Date: 2026-05-09
Status: Approved in chat

## Goal

Replace current Lua wrapper plugin layout based on `<plugin-id>.lua` plus optional `bootstrap.lua` with single bundle format that looks and behaves more like an `rqp` package while keeping plugin runtime simple.

New format must:

- use `run.lua` as only runtime entry file
- require `metadata.json`, `reqpack.lua`, and `run.lua`
- require `scripts/install.lua` and `scripts/remove.lua` as only package lifecycle hooks
- execute dependencies from `reqpack.lua` before install hook
- normalize repository names such as `rqp-plugin-bun` to local plugin id `bun`
- drop old plugin layout support completely

## Non-Goals

- support both old and new plugin layouts at same time
- expose configurable hook paths in `reqpack.lua`
- make plugin bundles full generic `.rqp` packages
- add separate bootstrap phase
- split runtime package-manager methods across multiple script files

## Target Bundle Layout

Each plugin is a directory bundle:

```text
plugins/<id>/
  metadata.json
  reqpack.lua
  run.lua
  scripts/
    install.lua
    remove.lua
```

Rules:

- `run.lua` is runtime entrypoint loaded by `LuaBridge`
- `metadata.json` is mandatory and defines canonical plugin id in `name`
- `reqpack.lua` is mandatory and contains only plugin package dependencies metadata
- `scripts/` directory is mandatory
- `scripts/install.lua` is mandatory and always used as install hook
- `scripts/remove.lua` is mandatory and always used as remove hook
- `<id>.lua` is invalid
- `bootstrap.lua` is invalid

## Canonical Identity

Canonical plugin id comes from `metadata.json.name`.

Example:

- source repository name: `rqp-plugin-bun`
- `metadata.json.name`: `bun`
- local installed directory: `plugins/bun/`
- runtime entry: `plugins/bun/run.lua`
- CLI system name: `bun`

Repository name is transport metadata only. ReqPack must not derive plugin id from repository directory or GitHub repository name once `metadata.json` is available.

If local bundle directory name disagrees with `metadata.json.name`, installation must fail unless ReqPack is in middle of materializing downloaded source into canonical directory. The final on-disk installed path must always be `plugins/<metadata.name>/`.

## `metadata.json`

Plugin bundles reuse familiar `rqp`-style metadata shape, but only fields needed for plugin identity and metadata are required.

Required fields:

```json
{
  "formatVersion": 1,
  "name": "bun",
  "version": "1.0.0",
  "summary": "ReqPack wrapper for Bun",
  "description": "Wrapper plugin that manages Bun packages through ReqPack",
  "license": "MIT"
}
```

Recommended optional fields:

- `sourceUrl`
- `homepage`
- `tags`

Not required for wrapper bundles:

- `release`
- `revision`
- `architecture`
- `payload`

Those fields may exist later, but implementation for this change must not depend on them.

## `reqpack.lua`

`reqpack.lua` is required and intentionally minimal.

It contains:

- `apiVersion = 1`
- `depends = { ... }`

It does not contain:

- hook path definitions
- runtime entry definition
- arbitrary package payload metadata

Example:

```lua
return {
  apiVersion = 1,
  depends = {
    "sys:git",
    "sys:curl@8.5.0",
  }
}
```

Dependency syntax uses normal ReqPack package spec strings such as:

- `sys:git`
- `sys:git@2.45.1`

ReqPack must parse these into package requests using existing package spec logic where possible.

## Runtime Contract

`run.lua` replaces both `<plugin-id>.lua` and `bootstrap.lua`.

It must expose the same global `plugin` table contract used by current Lua wrapper plugins. That keeps runtime behavior stable for package-manager operations such as:

- `install`
- `installLocal`
- `remove`
- `update`
- `list`
- `search`
- `info`
- optional existing plugin methods

No separate bootstrap file exists in new model. Any runtime readiness checks that were previously in `bootstrap.lua` move into `run.lua`, typically through `plugin.init()` or other runtime methods.

## Install Lifecycle

Plugin installation uses dedicated plugin bundle lifecycle, not old loose-script copy behavior.

Install flow:

1. fetch or locate plugin source bundle
2. validate presence and parseability of `metadata.json`, `reqpack.lua`, and `run.lua`
3. read canonical id from `metadata.json.name`
4. normalize materialization target to `plugins/<id>/`
5. parse dependencies from `reqpack.lua`
6. ensure missing dependencies are installed
7. execute `scripts/install.lua`
8. persist plugin installed state and manifest
9. register plugin so runtime loads `plugins/<id>/run.lua`

Critical rule:

- install hook always runs after dependencies are resolved and installed successfully

If dependency resolution or installation fails, `scripts/install.lua` must not run.

## Remove Lifecycle

Remove flow:

1. load installed plugin state
2. execute `scripts/remove.lua`
3. remove registered manifest artifacts in reverse order
4. delete persisted plugin state
5. remove installed runtime bundle under `plugins/<id>/`

If remove hook fails, ReqPack should stop and report failure rather than silently continuing.

## Hook Runtime Reuse

ReqPack should reuse existing `rqp` package hook runtime for `scripts/install.lua` and `scripts/remove.lua` instead of creating a second plugin-specific hook runtime.

Reason:

- runtime already supports `context.exec.run(...)`
- runtime already supports filesystem helpers
- runtime already supports artifact registration and manifest cleanup
- runtime already supports installed-state hook execution

This change should extract or adapt shared hook execution pieces so plugin bundles can use same machinery without pretending to be full `.rqp` payload packages.

Hook paths are fixed by convention:

- install hook: `scripts/install.lua`
- remove hook: `scripts/remove.lua`

No hook path lookup from `reqpack.lua` is needed. Both files are required.

## Installed State

Plugin installed state must track enough information to support removal, upgrades, and inspection.

Persist at least:

- canonical plugin id
- source metadata such as repository or source URL
- copied `metadata.json`
- copied `reqpack.lua`
- copied lifecycle scripts
- manifest of artifacts registered by install hook

Runtime copy must remain under active plugin directory:

- `plugins/<id>/run.lua`
- `plugins/<id>/metadata.json`
- `plugins/<id>/reqpack.lua`
- `plugins/<id>/scripts/*`

State storage for lifecycle bookkeeping may live in dedicated plugin state directory, but runtime should not depend on hidden state paths for loading plugin entrypoint.

## Registry and Downloader Changes

Current registry/downloader path assumes plugin payload is two loose files: main script and optional bootstrap script. New design changes payload handling from file pair to directory bundle.

Required changes:

- fetch whole plugin bundle directory
- validate bundle by checking required files
- read `metadata.json.name` as canonical id
- materialize to `plugins/<id>/`
- stop writing `<id>.lua`
- stop writing `bootstrap.lua`

For Git-backed sources like `rqp-plugin-bun`, ReqPack may clone repository cache using source repository name, but final installed output must still be canonical `plugins/bun/`.

## Discovery and Loading Changes

Discovery must change from scanning `.lua` files whose stem matches parent directory to scanning plugin directories that contain required bundle files.

A directory counts as plugin candidate only if it contains:

- `metadata.json`
- `reqpack.lua`
- `run.lua`

Loader rules:

- `LuaBridge` receives script path `plugins/<id>/run.lua`
- plugin id comes from parsed metadata, not from script stem `run`
- `bootstrapPath` is removed from runtime context and internal plugin state

## Test Runner Changes

`test-plugin` behavior must change:

- if user passes plugin directory, runner looks for `run.lua`
- if user passes plugin id, runner resolves `<plugin-dir>/run.lua`
- error messages mention missing `run.lua`, not missing `<id>.lua`

Hermetic test fixtures should migrate to new bundle structure.

## Error Handling

Installation must fail with explicit diagnostics when:

- `metadata.json` missing
- `metadata.json` invalid JSON
- `metadata.json.name` missing or empty
- `reqpack.lua` missing
- `reqpack.lua.apiVersion` missing or not `1`
- `reqpack.lua.depends` has invalid spec entries
- `run.lua` missing
- `scripts/install.lua` missing
- `scripts/remove.lua` missing
- dependency installation fails
- install hook fails

Runtime load must fail when bundle exists but required files are missing.

Old-format bundles must be rejected clearly instead of silently accepted.

## Compatibility Decision

This design intentionally breaks old wrapper plugin format.

ReqPack will no longer support:

```text
plugins/<id>/
  <id>.lua
  bootstrap.lua
```

Migration is explicit and complete. No fallback logic, no dual loader path, no deprecation bridge.

## Files and Components To Change

Expected code touchpoints:

- `src/main/cpp/plugins/lua_bridge.cpp`
- `src/main/include/plugins/iplugin.h`
- `src/main/cpp/core/registry/registry_database.cpp`
- `src/main/cpp/core/registry/registry_database_core.cpp`
- `src/main/cpp/core/registry/registry.cpp`
- `src/main/cpp/core/download/downloader.cpp`
- `src/main/cpp/core/download/downloader_core.cpp`
- `src/main/cpp/core/plugins/plugin_test_runner.cpp`
- `src/main/cpp/plugins/rq_plugin.cpp` or extracted shared hook runtime helper
- plugin template repository files
- wiki and README documentation
- integration and unit tests covering registry, loader, and plugin testing flows

## Testing Plan

Add or update tests for:

- plugin directory discovery using `run.lua`
- loader deriving plugin id from `metadata.json`
- registry normalization of `rqp-plugin-bun` to `bun`
- dependency installation before `scripts/install.lua`
- install hook not running if dependency install fails
- remove hook execution from installed state
- manifest cleanup after remove
- rejection of old plugin format
- `test-plugin` directory and id resolution using `run.lua`

## Recommended Implementation Order

1. define plugin bundle validation and metadata parsing helper
2. switch discovery and test-runner to `run.lua`
3. update `LuaBridge` to use bundle metadata id and remove bootstrap handling
4. adapt registry/downloader payload materialization to whole bundle directories
5. extract or reuse hook runtime from `rqp` package flow for plugin install/remove
6. add dependency parsing from `reqpack.lua` and enforce dependency-before-hook order
7. persist plugin installed state and manifest
8. migrate template, docs, and tests

## Open Questions Resolved

- only new format supported: yes
- runtime entry file: fixed `run.lua`
- lifecycle hooks: only `scripts/install.lua` and `scripts/remove.lua`
- dependency source: `reqpack.lua.depends`
- dependency order: always before install hook
- source repository name may differ from plugin id: yes, normalize to metadata name
- hook runtime reuse: yes, reuse existing `rqp` hook runtime
