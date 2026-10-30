# Agent quick reference (1 page)

Full detail: [AGENT.md](../../AGENT.md).

## Repos

| Dir | What |
| --- | --- |
| `reqpack/` | `rqp` binary, CLI, `output/*` (IDisplay) |
| `ReqPack-Core/` | orchestrator, planner, executor, registry, lua bridge, security bridge, audit/sbom export |
| `ReqPack-Core.wiki/` | Core library wiki (English) |
| `reqpack-security-core/` | OSV, validator, vulnerability DB/sync, security gateway (`rqp/security/*`) |
| `ReqPack.wiki/` | user + architecture wiki |

Dependency: `reqpack` → `ReqPack-Core` → `reqpack-security-core` (sibling path auto-detected by CMake).

## Proxy limits (planning)

- Max depth: **4**
- No cycles, no self-target, not both `packages` and `localPath`
- File: `ReqPack-Core/src/main/cpp/core/planning/request_resolution.cpp`

## Transaction run states

`open` → (`recovered` | `failed` | `committed`)

## Display item states (CLI only)

`PENDING` → `RUNNING` → (`SUCCESS` | `SKIPPED` | `FAILED`)

## Commands vs pipeline

| Commands | Pipeline |
| --- | --- |
| install, remove, update, ensure | full graph + optional validator |
| list, search, info, outdated | executor query + table |
| snapshot | history export, no plugins |
| audit, sbom | graph + export |
| serve --remote | + auth/framing |

## UI

Terminal only: `ColorDisplay` / `PlainDisplay`; JSON mode disables display.

## CLI

Full command tree + flags: [CLI-REFERENCE.md](./CLI-REFERENCE.md).
