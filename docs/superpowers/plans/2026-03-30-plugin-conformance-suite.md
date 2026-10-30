# Plugin Conformance Suite Implementation Plan

> **Goal:** Universal live certification for ReqPack plugins via `.rqp` install + standardized Lua live cases + parametrized CLI E2E phases.

**Architecture:** `rqp live run` mode `conformance` packs plugin on host, bootstraps container with `rqp install rqp`, runs `test-plugin --live`, then `run_live_conformance.sh` sourced from `profiles/<system>.env`.

**Status (2026-03-30):** Debian `debian-dpkg-conformance-all` green (P1–P7 incl. P6.4 with local OSV feed `fixtures/security/conformance-osv-feed.json`).

## Delivered

- C++ `test-plugin` expect: `maxItems`, `notContainsName`, `versionEquals`, `errorContains`
- `tests/system/live/run_live_conformance.sh` + `lib/conformance_common.sh`
- **Phase 1:** `run_live_conformance_p1.sh` (P1.1–P1.4)
- **Phase 2:** `run_live_conformance_p2.sh` (P2.1–P2.6 search/info), profile `profiles/dpkg-p2.env`, matrix row `debian-dpkg-conformance-p2`
- **Phase 3:** `run_live_conformance_p3.sh` (P3.1–P3.7 install/error tolerance), `profiles/dpkg-p3.env` / `profiles/nix-p3.env`, matrix rows `debian-dpkg-conformance-p3`, `nix-nix-conformance-p3` (Nix bootstraps local store path via `nix build nixpkgs#hello`)
- **Phase 4:** `run_live_conformance_p4.sh` (P4.1–P4.4 inventory), `profiles/dpkg-p4.env` / `profiles/nix-p4.env`, matrix rows `debian-dpkg-conformance-p4`, `nix-nix-conformance-p4` (Nix skips P4.3 group list via `CONFORMANCE_SKIP_P4_GROUP`)
- **Phase 5–7:** `run_live_conformance_p5.sh` … `p7.sh` (update, security/snapshot, remove); per-phase profiles + `dpkg-all.env` / matrix `debian-dpkg-conformance-all`
- `tests/system/live/profiles/{dpkg,dpkg-p2,rpm,pacman,npm}.env`
- `reqpack-plugins/d/dpkg/.reqpack-test/live/01–11` standard cases
- `main_live.cpp` mode `conformance`
- Fixture builder builds debian v1+v2 + corrupt stubs (requires `dpkg-deb` or container)

## Next

- Enable matrix `conformance` rows for rpm/npm/pacman after v2 fixtures + live case copies
- **Nix:** run `nix-nix-conformance-p3` after P2 green; offline `fixtures/nix/` optional (P3 uses bootstrap build)
- CycloneDX assertion in P6.1 (jq PURL check)
- Copy standard `.reqpack-test/live/` template to other plugins (nix: remote-only subset)
