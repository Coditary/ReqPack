# RQP Pack Command Design

Date: 2026-05-10
Status: Proposed in chat

## Goal

Add first-class `rqp pack` support so ReqPack can build installable `.rqp` artifacts directly from project directories, validate package structure before writing output, and optionally dispatch pack operations to package-manager plugins that expose native artifact build support.

This change must:

- add builtin `rqp pack <project-dir>` for `.rqp` output
- support three builtin input modes:
  - project directory with already-final `.rqp` control layout
  - project directory with embedded `payload-tree/`
  - project directory plus external payload directory via flag
- validate package metadata, hooks, payload structure, hashes, and archive layout before success
- write canonical `.rqp` output that matches current reader expectations
- allow plugins to optionally expose `pack` for native artifacts such as `.deb`, `.rpm`, or `.jar`
- keep builtin `.rqp` packing in core so format validation stays centralized

## Non-Goals

- add package scaffolding or project initialization commands
- support multi-package batch builds from one command
- redesign `.rqp` metadata schema beyond values already required by reader and current system metadata work
- require every plugin to implement `pack`
- add remote `pack` support in this change
- make pack operations part of planner dependency graphs or security vulnerability validation

## Current Context

Current code already contains most `.rqp` read-side rules in `src/main/cpp/core/packages/rq_package.cpp`:

- `metadata.json` parsing and normalization
- `reqpack.lua` parsing and hook validation
- payload hash verification
- zstd decompression
- tar parsing and outer archive path validation

Current integration tests also contain manual helper code that assembles `.rqp` packages by writing:

- `metadata.json`
- `reqpack.lua`
- optional `payload/payload.tar.zst`
- optional `hashes/payload.sha256`
- outer tar archive with `.rqp` extension

That helper proves format expectations already exist and should be moved into production code rather than duplicated again in tests.

Current command architecture has two relevant patterns:

- graph/planner/executor flow for install-like actions
- direct command services such as `snapshot` that bypass planner and write output directly

`pack` is build-time artifact creation, not package resolution or dependency execution. It should follow direct-service pattern, not graph execution.

## CLI

### Builtin `.rqp` pack

```text
rqp pack <project-dir> [--output <path>] [--payload-dir <path>] [--force]
```

Behavior:

- one positional argument after `pack` means builtin `.rqp` mode
- `<project-dir>` must be directory
- `--output` overrides output artifact path
- `--payload-dir` points to external payload tree and is optional
- `--force` overwrites existing output file without prompt

### Plugin pack

```text
rqp pack <system> <project-dir> [--output <path>] [--force] [plugin flags...]
```

Behavior:

- two positional arguments after `pack` means plugin mode
- first positional must resolve to known system/plugin id
- second positional must be directory path for plugin-owned build input
- plugin may build native artifact format
- `--output` is passed through when plugin supports explicit target output path
- when plugin does not support pack, ReqPack returns explicit error

### Parsing Rules

- `rqp pack <project-dir>` always selects builtin mode
- `rqp pack <system> <project-dir>` selects plugin mode only when first token resolves to known system
- ambiguous two-arg form where first token is not known system is an error, not fallback magic
- builtin mode does not require host/package-manager resolution

### Help Examples

```text
rqp pack ./my-package
rqp pack ./my-package --output ./dist/demo.rqp
rqp pack ./my-package --payload-dir ./build/rootfs
rqp pack deb ./debian-project --output ./dist/demo.deb
```

## Builtin Project Layouts

Builtin `.rqp` pack supports exactly one of these payload modes.

### Mode A: control-only package

```text
project/
  metadata.json
  reqpack.lua
  scripts/
```

Result:

- `.rqp` contains only control files
- `metadata.payload` must be absent
- output package installs without payload archive

### Mode B: prebuilt final payload

```text
project/
  metadata.json
  reqpack.lua
  scripts/
  payload/
    payload.tar.zst
  hashes/
    payload.sha256
```

Result:

- builtin pack validates prebuilt payload and hash
- canonical output is rebuilt from validated inputs

### Mode C: embedded payload tree

```text
project/
  metadata.json
  reqpack.lua
  scripts/
  payload-tree/
```

Result:

- builtin pack creates `payload/payload.tar.zst`
- builtin pack creates `hashes/payload.sha256`
- builtin pack fills canonical `metadata.payload`

### Mode D: external payload tree

```text
rqp pack ./project --payload-dir ./rootfs
```

Result:

- same build behavior as Mode C
- payload source comes from external directory

### Conflict Rules

- `--payload-dir` is mutually exclusive with project `payload-tree/`
- `payload-tree/` is mutually exclusive with prebuilt `payload/` or `hashes/`
- prebuilt `payload/` and `hashes/` must appear together
- control-only package must not contain payload files or payload metadata

These rules intentionally reject ambiguous input instead of choosing precedence silently.

## Builtin Output Rules

### Default Output Path

When `--output` is omitted, builtin `.rqp` output path is:

```text
<project-dir-parent>/<metadata.name>.rqp
```

Reasons:

- minimal and predictable
- keeps generated artifact out of source input tree by default
- matches current test helper naming convention closely

### Overwrite Behavior

- if output path exists and `--force` not set, ReqPack prompts before overwrite in interactive mode
- in non-interactive mode without `--force`, command fails with explicit diagnostic

### Canonical Write Rules

Builtin pack always writes canonical package contents, not raw byte-for-byte copies of source files.

Canonical output must:

- write normalized `metadata.json` via shared metadata serializer
- normalize missing architecture to `noarch`
- normalize missing system to `nosys`
- write `system` as canonical JSON array
- write generated or validated `metadata.payload` fields in canonical supported form
- include only paths accepted by current reader

## Validation Model

Builtin pack must validate before reporting success.

### Metadata Validation

Use existing reader-side metadata parser rules:

- required core fields must exist
- architecture/system normalization follows current logic
- payload metadata must match supported shape when present

Unsupported-but-known-invalid payload metadata must fail early, for example:

- wrong payload path
- wrong archive or compression values
- wrong hash file path
- malformed hash content

### Hook Validation

Use existing `reqpack.lua` parsing and hook validation rules:

- manifest must parse as Lua
- `apiVersion` must be supported
- install hook must exist
- referenced script paths must stay inside package structure

### Source Layout Validation

Builtin pack must validate reserved package-build inputs without requiring project directory to contain only package-build inputs.

Reserved source entries:

- `metadata.json`
- `reqpack.lua`
- `scripts/`
- `payload/`
- `hashes/`
- `payload-tree/`

Rules:

- reserved entries are interpreted by pack command
- non-reserved files and directories are ignored and never embedded automatically
- conflicting reserved combinations still fail explicitly

This keeps real project directories usable while still rejecting ambiguous package input.

### Payload Validation

For prebuilt payload mode:

- verify `payload/payload.tar.zst` exists
- verify `hashes/payload.sha256` exists
- verify sha256 matches archive bytes
- decompress payload archive
- parse tar entries and validate paths using existing extraction rules

For payload-tree mode:

- walk source tree recursively
- reject invalid relative paths, absolute paths, `.` or `..` segments
- support regular files, directories, and symlinks only
- compute installed size from resulting unpacked payload tree
- tar and zstd-compress payload internally
- compute sha256 and write canonical hash file

### Final Self-Validation

After builtin pack writes `.rqp`, ReqPack should re-open produced archive through shared structural validation logic.

Important exception:

- final self-validation must skip host compatibility rejection for architecture/system matching

Reason:

- package build must allow cross-target package creation on non-target hosts
- format validity and host installability are separate concerns

## Builder Architecture

Add new core builder service for `.rqp`, for example `RqPackageBuilder`.

Responsibilities:

1. resolve project input mode
2. validate source layout
3. load and normalize metadata
4. validate `reqpack.lua`
5. generate or validate payload material
6. assemble canonical control tree in temp directory
7. write outer `.rqp` archive
8. run final structural self-validation

### Archive Writing

Preferred implementation:

- add internal tar writer in core
- continue using linked zstd library for compression

Reason:

- avoids runtime dependence on external `tar` and `zstd` executables
- keeps pack behavior portable across supported platforms
- allows deterministic ordering and tighter validation control

Supported tar entry types for builder should match what reader already accepts for extraction semantics:

- regular file
- directory
- symlink

Entry ordering should be deterministic by lexicographic path order.

## Direct Command Flow

`pack` should not go through planner, validator, or install executor graph.

Add direct service path in orchestrator similar to `snapshot`, for example `PackService` or `PackCommandRunner`.

Responsibilities:

- interpret builtin vs plugin pack mode from parsed request
- resolve plugin when plugin mode requested
- run builtin builder or plugin pack callback
- collect artifact information for output
- emit diagnostics and artifact blocks

## Plugin Pack Extension

Plugin pack support is optional.

### C++ Plugin Interface

Extend `IPlugin` with default non-breaking virtuals:

- `virtual bool supportsPack() const { return false; }`
- `virtual bool pack(const PluginCallContext& context, const std::string& projectPath, const std::string& outputPath, const std::vector<std::string>& flags) { return false; }`

Default implementations keep current plugins source-compatible.

### Lua Plugin Interface

LuaBridge should expose optional function:

```lua
function plugin.pack(context, projectPath, outputPath, flags)
  -- build artifact, register artifacts, return boolean success
end
```

`supportsPack()` in LuaBridge returns true when `plugin.pack` exists and is callable.

### Artifact Reporting

Plugin pack should reuse existing artifact registration flow.

- plugin returns boolean success
- plugin reports produced artifacts through `context.artifacts.register(...)`
- if `--output` was given, plugin should write there and register that artifact
- if `--output` was omitted, plugin may choose default location but must register at least one artifact on success

If plugin pack succeeds without output path and registers no artifact, ReqPack should treat that as failure because command result would be opaque.

### Output Ownership

- builtin `.rqp` mode owns `.rqp` packaging rules entirely in core
- plugin pack owns native artifact format details entirely in plugin
- system `rqp` may optionally expose plugin `pack`, but builtin `rqp pack <project-dir>` remains canonical path for `.rqp`

## Runtime Write Scope For Plugin Pack

Plugin pack may need to write build artifacts and temporary files near project input.

ReqPack should grant explicit runtime-local write allowance for:

- `projectPath`
- output file path or its parent directory
- plugin temp directory created through runtime host

This allowance should apply only for active `pack` call and should not permanently change plugin trust metadata.

Declared plugin security metadata still governs unrelated write locations and exec/network behavior.

## Request Model

Add `ActionType::PACK`.

Reuse existing `Request` fields where possible:

- `system`
- `localPath` for project directory
- `outputPath`
- `flags`

Add one new request field for builtin external payload source:

- `payloadPath`

Reason:

- overloading `localPath` for both project dir and external payload dir would be ambiguous

## Error Handling

Errors should be explicit and actionable.

Examples:

```text
pack project is missing metadata.json
```

```text
pack input is ambiguous: use either payload-tree/ or --payload-dir, not both
```

```text
system 'deb' does not support pack
```

```text
plugin pack succeeded but did not register any output artifact
```

```text
output file already exists: /path/to/demo.rqp
Use --force to overwrite.
```

## Output Model

Successful builtin pack should emit concise artifact-oriented output.

Recommended fields:

- `System`
- `Format`
- `Package`
- `Version`
- `Output Path`

Plugin pack should emit:

- `System`
- `Artifacts`
- artifact block entries for registered outputs

## Testing

### Unit Tests

- CLI parsing for builtin and plugin pack forms
- conflict detection for payload modes
- external payload dir handling
- metadata canonicalization on write
- final self-validation skipping host compatibility only
- plugin support detection for `pack`

### Integration Tests

- `rqp pack <project-dir>` builds control-only `.rqp`
- `rqp pack <project-dir>` with `payload-tree/` builds installable `.rqp`
- `rqp pack <project-dir> --payload-dir <dir>` builds installable `.rqp`
- prebuilt payload mode validates hashes and preserves installability
- ambiguous source layouts fail
- overwrite prompt and `--force` behavior
- produced `.rqp` can be installed by existing `rqp install <file.rqp>` flow
- plugin mode dispatches to Lua plugin `pack`
- plugin mode errors when plugin lacks `pack`
- plugin mode fails when no artifact registered on success without explicit output

### Regression Coverage

- existing `.rqp` install tests continue to pass
- current package reader behavior remains source of truth for accepted package format

## Open Design Decisions Resolved

- builtin CLI shape: `rqp pack ...`
- plugin CLI shape: `rqp pack <system> <project-dir>`
- builtin `.rqp` output stays in core
- plugin pack remains optional
- plugin pack may build native artifacts, not only `.rqp`
- external payload source is supported through `--payload-dir`

## Recommended Implementation Order

1. add `ActionType::PACK`, CLI parse, help text, and request field for payload path
2. extract current test-only `.rqp` build knowledge into core builder utilities
3. implement builtin `RqPackageBuilder` and final self-validation path
4. add orchestrator direct service for pack output
5. extend plugin interface and LuaBridge with optional `pack`
6. add plugin pack dispatch and artifact validation
7. add unit and integration coverage for builtin and plugin pack modes
