# RQP System Metadata And Build Version Design

Date: 2026-05-10
Status: Approved in chat

## Goal

Extend `.rqp` package metadata and runtime behavior so ReqPack can restrict packages to matching operating systems or distro families, display sane defaults for missing metadata, and report build release identity through `rqp version` and `rqp --version`.

This change must:

- add package metadata support for system targeting
- support single or multiple target systems in `metadata.json`
- match packages against host distro, host OS family, and alias groups
- allow users to define custom system aliases in config for niche distros or local naming
- default missing package architecture to `noarch`
- default missing package system to `nosys`
- include release identity in release builds and expose it through CLI version output
- keep metadata representation safe for later persistence and LMDB-backed state usage

## Non-Goals

- redesign ReqPack manifest package syntax
- replace existing planner system alias feature used for command routing
- add semver build metadata formatting to CLI version output
- add remote API changes beyond what current CLI and state handling need
- solve cross-distro package compatibility beyond explicit tokens and alias groups

## Current Constraints

Current code expects `.rqp` metadata fields such as `release`, `revision`, `architecture`, `vendor`, `maintainerEmail`, and `url` to be present.

Current code also has a separate `planner.systemAliases` config map used for command-time system alias expansion such as `yum -> dnf`. That feature must remain unchanged. Package compatibility aliases are a different concern and must use a separate config field under `rqp` to avoid semantic collision.

Current CLI version output uses `config.version`, while other places still contain hardcoded version strings such as `ReqPack/0.1.0` and builtin plugin version `1.0.0`. This change should centralize build identity enough that visible version output stops drifting.

## Metadata Model

`.rqp` metadata gains new logical field `system`.

Accepted JSON forms:

```json
{
  "system": "debian"
}
```

```json
{
  "system": ["debian", "ubuntu", "linux"]
}
```

Internal normalized form:

- store system targets as lowercase deduplicated list
- missing `system` becomes `["nosys"]`
- empty string values are ignored
- empty list after normalization becomes `["nosys"]`

Architecture normalization:

- missing or empty `architecture` becomes `noarch`

Canonical examples:

```json
{
  "formatVersion": 1,
  "name": "demo",
  "version": "1.0.0",
  "release": 1,
  "revision": 0,
  "summary": "Demo package",
  "description": "Demo package with distro gating",
  "license": "MIT",
  "architecture": "noarch",
  "system": ["debian", "ubuntu", "linux"],
  "vendor": "ReqPack",
  "maintainerEmail": "team@example.org",
  "url": "https://example.org/demo.rqp"
}
```

```json
{
  "formatVersion": 1,
  "name": "portable",
  "version": "1.0.0",
  "release": 1,
  "revision": 0,
  "summary": "Portable package",
  "description": "Portable package for any host",
  "license": "MIT",
  "vendor": "ReqPack",
  "maintainerEmail": "team@example.org",
  "url": "https://example.org/portable.rqp"
}
```

Second example normalizes to:

- `architecture = "noarch"`
- `systems = ["nosys"]`

## Host Match Model

ReqPack already gathers host OS metadata through host snapshot logic. Package compatibility should derive host tokens from that snapshot instead of inventing a second detection flow.

Derived host tokens:

- Linux Debian host: `debian`, `linux`
- Linux Fedora host: `fedora`, `linux`
- macOS host: `macos`, `darwin`
- Windows host if later supported by `.rqp`: `windows`

Rules:

- `nosys` matches every host
- exact token match succeeds
- generic `linux` matches any Linux distro
- `macos` and `darwin` both match macOS hosts
- alias-group overlap also matches

Package is installable when at least one normalized package system token matches at least one host token directly or through alias expansion.

Package is installable by architecture when normalized architecture is `noarch` or equals host architecture.

Package must satisfy both:

- architecture match
- system match

## Alias Groups

System targeting needs richer grouping than direct string equality. Built-in alias groups provide common family names. User config may extend or override alias groups for local or niche distro naming.

Examples of built-in alias groups:

- `debian-family = ["debian", "ubuntu", "linuxmint", "pop"]`
- `rhel-family = ["rhel", "rocky", "almalinux", "centos", "fedora"]`
- `darwin-family = ["darwin", "macos"]`

Matching behavior:

- if package targets `debian-family`, Ubuntu host matches
- if package targets `ubuntu`, alias expansion can still make it part of `debian-family` for group matching
- if package targets both `debian` and `fedora`, either branch is enough

Normalization rules:

- lowercase all alias names and members
- deduplicate members
- drop empty values
- alias names may themselves appear in package metadata

Cycle handling:

- aliases should expand through graph traversal
- cycles must not recurse forever
- self-reference is ignored after first visit

## Config Model

Add new config field under `rqp` namespace:

```lua
rqp = {
  systemAliases = {
    ["debian-family"] = { "debian", "ubuntu", "linuxmint", "pop" },
    ["my-lab-linux"] = { "nobara", "fedora" },
  }
}
```

Why under `rqp`:

- `planner.systemAliases` already exists and means command routing aliases like `yum -> dnf`
- package compatibility aliases are different data with different structure
- new field avoids ambiguity and migration risk

Merge semantics:

- built-in alias map loads first
- user config merges on top
- if user defines existing alias key, resulting member set is union of builtin and user values
- if user defines new alias key, new group is added

This keeps builtin convenience while allowing site-specific expansion for niche distros.

## Data Structures

`RqMetadata` changes:

- keep existing string field for `architecture`, but normalize default to `noarch`
- add `std::vector<std::string> systems`

Do not store multi-value system data as comma-separated string in metadata structs or persisted state. Multi-value lists are clearer, safer to compare, and better suited for future LMDB-backed persistence because serialization boundaries remain explicit.

If compatibility with existing JSON copies of installed metadata matters, metadata JSON written to disk may preserve original field shape or choose canonical array form. For internal logic, array/list form is authoritative.

Recommended canonical persisted shape:

- always write `system` as JSON array for installed-state metadata copies
- still accept legacy string-or-array on read

This gives stable persistence while remaining backward compatible at parser boundary.

## Installation And Resolution Behavior

`.rqp` load and install flow changes:

1. parse `metadata.json`
2. normalize `architecture` to `noarch` when blank or absent
3. normalize `system` to list, default `nosys`
4. build host token set from host snapshot
5. expand alias groups from builtin plus config
6. reject package if architecture mismatch
7. reject package if system mismatch

Error messages should be explicit. Example shape:

```text
package system does not match host
package systems: debian-family, ubuntu
host systems: fedora, linux
```

Repository resolution changes:

- repository package index should gain package `system` metadata in same change
- resolver should filter by system as well as architecture

For this change, design target is to support system-aware resolution for `.rqp` packages listed in repository indexes, not only local file installs. This is not deferred future work; repository schema and resolver updates are part of same implementation scope.

## Listing And Info Output

When ReqPack lists installed `.rqp` packages:

- if normalized architecture missing from source metadata, show `noarch`
- if normalized systems missing from source metadata, show `nosys`

Display behavior:

- single system token shows as plain value, e.g. `debian`
- multiple system tokens show comma-separated value, e.g. `debian, ubuntu, linux`

`PackageInfo.system` already means package-manager identifier such as `apt`, `dnf`, or `rqp`. That meaning must not change.

Package compatibility targets should therefore be exposed separately, for example by:

- new dedicated `PackageInfo` field such as `targetSystems`, or
- `extraFields` for detail output plus optional extra table column for list output

Implementation should choose one consistent representation, but package-manager `system` and package compatibility `system` must stay distinct.

Expected output behavior:

- `rqp list rqp` table shows architecture column as `noarch` when absent
- `rqp list rqp` table shows package compatibility targets in dedicated column if list layout is extended
- `rqp info rqp <package>` or equivalent detail output includes dedicated compatibility field such as `Target Systems` with `nosys` default

## CLI Version Output

ReqPack should report release build identity rather than current config placeholder version.

Commands:

- `rqp version`
- `rqp --version`

Both must print only release identity.

Examples:

```text
ReqPack v1.2.3
```

```text
ReqPack 08d36ff
```

Local builds without injected release identity use fallback such as:

```text
ReqPack dev
```

## Build Integration

Release workflow already computes `RELEASE_ID`. Build should pass that into CMake and compiled code.

Preferred build model:

- add compile definition or generated header for build release id
- default to `dev` when not supplied
- use same build release id for:
  - CLI version output
  - builtin `rqp` plugin version
  - downloader user agent if practical for this change

This removes drift between `config.version`, builtin plugin `getVersion()`, and hardcoded user-agent strings.

## Error Handling

Parser errors:

- reject non-string, non-array `system`
- reject array members that are not strings
- reject unsupported `formatVersion`

Runtime errors:

- architecture mismatch
- system mismatch
- invalid alias expansion should fail safe without recursion loops

Config handling:

- malformed `rqp.systemAliases` entries should produce config parse error
- empty alias groups are ignored

## Testing

Unit tests:

- metadata parser accepts `system` as string
- metadata parser accepts `system` as array
- missing `system` defaults to `nosys`
- missing `architecture` defaults to `noarch`
- invalid `system` type rejects
- alias expansion handles cycles safely
- exact distro match succeeds
- generic `linux` match succeeds on Linux distro
- `darwin` and `macos` match macOS
- config alias merge adds user members to builtin groups

Integration tests:

- local `.rqp` install succeeds on matching distro token
- local `.rqp` install fails on mismatched distro token
- repository resolve filters by system and architecture
- `list` shows `noarch` and `nosys` defaults
- `info` shows normalized system field
- `rqp version` prints injected release id
- `rqp --version` prints injected release id

Regression tests:

- existing `planner.systemAliases` behavior still works unchanged
- existing packages with explicit `architecture = "noarch"` still install
- packages without `system` remain installable everywhere through `nosys`

## Open Implementation Notes

- `planner.systemAliases` and `rqp.systemAliases` must stay separate in config parsing and naming
- host system token builder should reuse cached host snapshot APIs instead of direct `/etc/os-release` reads in package code
- persisted installed metadata should move toward canonical array form for `system` while parser remains tolerant
- repository index schema extension for package `system` is part of this change, not a later follow-up

## Acceptance Criteria

- `.rqp` metadata accepts `system` as string or array
- missing `system` behaves as `nosys`
- missing `architecture` behaves as `noarch`
- install rejects incompatible package systems
- alias groups support builtin and user-defined entries under `rqp.systemAliases`
- `rqp list` and package detail output show normalized defaults
- `rqp version` and `rqp --version` print compiled release identity
- no regression in existing planner alias behavior
