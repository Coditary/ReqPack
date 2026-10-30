# CLI Command-Baum und Flags (Agent-Referenz)

**Quellen im Code (bei Abweichungen diese Dateien gewinnen):**

| Bereich | Datei |
| --- | --- |
| Command-Liste & Hilfetexte | `reqpack/src/main/cpp/cli/cli_help_text.cpp` |
| Token → Action | `ReqPack-Core/src/main/cpp/core/common/action_tokens.cpp` |
| Request-Parsing | `reqpack/src/main/cpp/cli/cli_parse_core.cpp` |
| Manifest-/Target-Hilfen | `reqpack/src/main/cpp/cli/cli_parse_targets.cpp` |
| Request-Flag-Normalisierung | `reqpack/src/main/cpp/cli/cli_parse_request_flags.cpp` |
| Plugin-Cluster | `reqpack/src/main/cpp/cli/cli_plugin_commands.cpp` |
| Globale Config-Flags | `ReqPack-Core/src/main/cpp/core/config/configuration_cli_overrides.cpp` |
| Dispatch (Sonderpfade) | `reqpack/src/main/cpp/main_dispatch.cpp` |

**Syntax-Grundform:**

```text
rqp [<globale-flags>...] <command> [targets...] [<options>...]
```

Viele Flags sind **global** (werden aus argv entfernt und in `ReqPackConfig` gemerged). Command-Flags landen in `Request.flags` (ohne `--`, z. B. `all`, `force`, `wide`).

---

## 1. Command-Baum (Übersicht)

```text
rqp
├── -h, --help / --help-all       → Kurz- vs. Vollhilfe
├── -V, --version | version        → Versionsstring
├── -v, --verbose                 → Logger-Konsole an
├── -y, --yes                     → Nicht-interaktiv (Alias --non-interactive)
├── -c, --config <path>           → Alternatives config.lua
├── --json                        → JSON auf stdout
│
├── install (i)                   → Pakete / Manifest / lokale Targets
├── remove (rm)                     → Wie install, Aktion REMOVE
├── update (up)                     → Paket-Updates (Manifest, scoped, --all)
├── search (s, find)                → Ein System + Query
├── list (ls)                       → Installierte Pakete
├── outdated (out)                  → Veraltete Pakete
├── info                            → rqp / Plugin / Paket-Metadaten
│
├── snapshot                        → installierten Zustand → reqpack.lua
├── pack                            → .rqp oder plugin-native Artefakte
├── audit                           → Vulnerability-Audit + Export
├── sbom                            → SBOM exportieren
├── security status|doctor          → OSV-DB / Mapping-Diagnose
│
├── plugin update|ensure|test|info  → Wrapper & Plugin-Tooling
├── self-update                     → rqp-Binary-Release (empfohlen)
├── ensure                          → Alias für plugin ensure
├── test-plugin                     → Alias für plugin test
│
├── host refresh                    → Host-Metadaten-Cache
├── serve --stdin | --remote        → Batch / TCP-Server
├── remote <profile> [cmd...]       → Remote-Client
│
└── [<globale Security-/Log-Flags>] → überall mitschreibbar
```

**Keine CLI11-Subcommands:** Erstes Action-Token = Command. `host`, `remote`, `serve`, `security`, `self-update`, `plugin …`, `version`, `test-plugin` haben Sonderpfade in `main_dispatch.cpp` bzw. frühe Parser.

---

## 2. Commands → Laufzeitpfad

| Command | Alias | Runtime |
| --- | --- | --- |
| `install` | `i` | Orchestrator (mutating) |
| `remove` | `rm` | Orchestrator |
| `update` | `up` | Orchestrator (Pakete); leer → **Self-update** (deprecated) |
| `self-update` | — | `run_self_update` |
| `plugin update` | — | Wrapper-Refresh (wie früher `update <sys>`) |
| `plugin ensure` | `ensure` | Orchestrator ENSURE |
| `plugin test` | `test-plugin` | `run_plugin_test_cases` (presets `core`, `live`; flag `--live`) |
| `plugin info` | — | Orchestrator INFO |
| `plugin update-tool` | — | `update sys <tool>` |
| `search` | `s`, `find` | Orchestrator → Tabelle |
| `list` | `ls` | Fast path |
| `outdated` | `out` | Fast path |
| `info` | — | Lokal ohne Args; sonst Orchestrator |
| `audit` / `sbom` | — | Orchestrator + Export |
| `security status` | — | `run_security_status` |
| `security doctor` | `audit --doctor` | wie status |
| `snapshot` / `pack` | — | Export / Pack flow |

---

## 3. Einheitliche Target-Auflösung

Gilt für: **install, remove, update, list, outdated, info, audit, sbom** (`uses_unified_package_targets` / `cli_parse_targets.cpp`).

| Target | Beispiel |
| --- | --- |
| System + Pakete | `rqp install apt curl git` |
| Scoped | `apt:curl` wenn `apt` bekannt |
| Verzeichnis | `.` / `./proj` → `reqpack.lua` im Ordner |
| Manifest-Datei | `./reqpack.lua` |
| URL / lokale Datei | **install** only (pro System) |

**Regeln:** Pro System nicht Paketnamen und Local-Path mischen. Manifest-Modus lädt alle `packages`-Einträge.

### Update-Semantik (neu)

| Aufruf | Wirkung |
| --- | --- |
| `rqp update` / `rqp up` | Self-update (+ Deprecation-Hinweis) |
| `rqp self-update` | Self-update |
| `rqp update <sys>` | **Alle Pakete** des Systems (`all` intern) |
| `rqp update <sys> --all` / `-a` | Explizit alle Pakete |
| `rqp update --plugins [sys]` | Wrapper-Refresh (legacy unter `update`) |
| `rqp plugin update [sys]` | Wrapper-Refresh |
| `rqp update --all` (ohne sys) | Alle Plugin-Wrapper |
| `rqp update sys <tool>` | PM-Binary via sys-Schicht |

---

## 4. Globale Flags (Auszug)

Siehe `rqp --help-all` und `configuration_cli_overrides.cpp`.

| Kurz | Lang | Wirkung |
| --- | --- | --- |
| `-n` | `--dry-run` | Dry-run |
| `-y` | `--yes`, `--non-interactive` | Keine Prompts |
| `-c` | `--config` | Config-Pfad |
| `-j max` / `-j 0` | `--jobs-max` | Alle CPU-Threads |
| | `--on-unsafe` | `prompt\|abort\|ignore` |
| | `--on-unresolved` | `prompt\|abort\|ignore` |
| | `--prompt-on-unsafe` / `--abort-on-unsafe` | Aliase |

Logging kompakt in Hilfe: `--[no-]log-console`, `--[no-]log-capture-display`, `--[no-]fail-on-findings`.

---

## 5. Command-Flags (Kurzformen)

| Command | Kurzflags in Parser |
| --- | --- |
| `update` | `-a` → `all`, `--plugins` |
| `audit`, `sbom`, `snapshot`, `pack` | `-o` → output path, `-f` → `force` |
| `pack` (Metadaten) | `--pack-*` → `Request.packMetadata`; Env `REQPACK_PACK_*` füllt fehlende Felder beim Build |
| `install`, `remove`, `update` | `-n` → `dry-run` in Request.flags (+ global) |
| `search`, `list`, `outdated` | `--show-arch` (statt `-v` für Arch-Spalte) |

---

## 6. Hilfe

```bash
rqp --help
rqp --help-all
rqp update -h
rqp security --help
rqp plugin --help
rqp version --help
```

Enduser-Detail: `ReqPack.wiki/User-Guide-Command-Reference.md`.

### `test-plugin` / `plugin test`

| Flag / Preset | Wirkung |
| --- | --- |
| `--preset core` | Hermetisch: Cases aus `.reqpack-test/core/`, `fakeExec` Pflicht |
| `--live` | Preset `live`: Cases aus `.reqpack-test/live/`, echte Exec (Container) |
| `--preset live` | Wie `--live` |
| `expect.minItems` | Mindestanzahl Ergebniszeilen (list/search/outdated/info) |
| `expect.containsName` | Paketname muss in Ergebnis vorkommen |

Beispiel Live: `rqp test-plugin --plugin dpkg --live --report /tmp/report.json`
