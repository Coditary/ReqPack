# ReqPack

**ReqPack** (short for "Request-based Package Manager") is a universal, modular meta package manager that orchestrates existing language-specific package managers like `pip`, `npm`, `cargo`, or `maven`. Instead of managing packages itself, ReqPack intelligently delegates tasks, downloads missing tools, and auto-installs its own language-specific logic via Lua scripts.

---

## ✨ Features

* 🧠 **Smart Language Routing**
  Install any package with a single command. ReqPack detects the target language and delegates the request to the proper package manager.

* 🧹 **Lua-Based Modular System**
  Each supported language is handled by a standalone Lua downloader script. This makes adding new languages or platforms simple and lightweight.

* 🌐 **Decentralized, Versioned Registry**
  All plugin locations are stored in a versioned registry, which can be synced from remote sources and merged locally.

* 🚀 **Lazy Loading & Auto Bootstrapping**
  If a required package manager (like `pip` or `npm`) is missing, ReqPack automatically downloads and installs it alongside its Lua logic.

* 🛡️ **Safe Local Extensions**
  You can register your own package managers locally without worrying about them being overwritten by remote updates.

* 📋 **SBOM Generation**
  Generate a Software Bill of Materials (SBOM) in CycloneDX format for any set of packages to support supply-chain transparency and compliance workflows.

* 🔎 **Dependency Audit Reports**
  Audit planned packages against vulnerability data and export findings as terminal tables, CycloneDX VEX JSON, or SARIF.

---

## 🍞 Example Usage

```bash
reqpack install pip flask
reqpack install npm express
reqpack install java lombok
reqpack update
reqpack update --all
reqpack update pip
reqpack update pip --all
reqpack update sys pip
```

ReqPack will detect whether Python or `pip` are installed, download them if missing, then proceed with the installation using its Lua plugin.
When `reqpack update` is called without a system, ReqPack updates itself from its configured Git repository, builds a fresh local binary, and repoints the local `rqp` symlink.
When `reqpack update --all` is called, ReqPack refreshes all known plugin wrappers.
When `reqpack update <plugin>` is called without package names, ReqPack refreshes that plugin wrapper itself. For Git-backed plugin sources, it selects the newest tagged version and rematerializes the plugin locally.
When `reqpack update <system> --all` is called, ReqPack updates all packages for that system.
When a package-manager binary itself should be updated through ReqPack's wrapper layer, use `reqpack update sys <tool>`, for example `reqpack update sys pip`.

### Test Coverage And Profiling

```bash
# Build instrumented tests, run them, and print a coverage summary
make test-coverage

# Build profiling binaries, run tests through perf when available,
# and print hottest symbols from the captured run
make profile-tests
```

Coverage writes CTest output under `build/coverage/Testing/.../Coverage.xml` and prints a summary for files in `src/main/cpp`.
Profiling writes raw data and text reports under `build/profile/profile-data/`. On Linux, `perf` is preferred and profiles each test binary directly without inheriting child processes, which keeps the hotspot list focused on ReqPack code instead of external helper tools. If `perf` is unavailable, ReqPack falls back to repeated `gprof` runs for the unit-test binaries.

---

## 📁 File Locations

ReqPack follows XDG base directory rules.

### Config

- `$XDG_CONFIG_HOME/reqpack/config.lua`
- `$XDG_CONFIG_HOME/reqpack/remote.lua`
- Fallback when `XDG_CONFIG_HOME` is unset or empty: `~/.config/reqpack/...`

### Data

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
- Fallback when `XDG_DATA_HOME` is unset or empty: `~/.local/share/reqpack/...`

### Local Binary Link

- Self-update symlink default: `~/.local/bin/rqp`

### Cache

- `$XDG_CACHE_HOME/reqpack/transactions`
- `$XDG_CACHE_HOME/reqpack/security/cache`
- Fallback when `XDG_CACHE_HOME` is unset or empty: `~/.cache/reqpack/...`

---

## 📋 SBOM Generation

ReqPack can generate a **Software Bill of Materials** for any set of packages using the `sbom` command. The output follows the [CycloneDX 1.5](https://cyclonedx.org/) standard and includes component metadata, PURL identifiers, and optional dependency edges.

### Basic Usage

```bash
# SBOM for specific packages — prints CycloneDX JSON to stdout
reqpack sbom maven org.junit:junit:4.13 com.google.guava:guava:32.1.3-jre

# Scoped syntax (system:package)
reqpack sbom maven:org.junit:junit:4.13

# Multiple systems in one invocation
reqpack sbom maven org.junit:junit pip flask numpy
```

### Output Formats

| Flag | Value | Description |
|------|-------|-------------|
| `--format` | `cyclonedx-json` | CycloneDX 1.5 JSON *(default when `--output` is set)* |
| `--format` | `json` | ReqPack-native JSON |
| `--format` | `table` | Fixed-width text table |

### Writing to a File

```bash
# Write CycloneDX JSON to a file (format inferred automatically)
reqpack sbom maven org.junit:junit --output bom.json

# Explicit format + output path
reqpack sbom pip flask --format cyclonedx-json --output reports/sbom.json
```

### Table Layout

```bash
# Wider table layout for terminal output
reqpack sbom --wide

# Keep each row on one line and let terminal handle overflow
reqpack sbom --no-wrap
```

### CycloneDX Output Example

```json
{
  "bomFormat": "CycloneDX",
  "specVersion": "1.5",
  "version": 1,
  "components": [
    {
      "type": "library",
      "bom-ref": "maven:org.junit:junit@4.13",
      "name": "org.junit:junit",
      "version": "4.13",
      "purl": "pkg:maven/org.junit/junit@4.13",
      "properties": [
        { "name": "reqpack:system", "value": "maven" }
      ]
    }
  ]
}
```

If `includeDependencyEdges` is enabled in the ReqPack config, a `dependencies` array is appended listing `dependsOn` relationships between components.

---

## 🔎 Audit

ReqPack can audit planned packages with the `audit` command. It uses the same package input styles as `install`, generates component data internally, matches vulnerabilities, and exports findings as either a terminal table, CycloneDX VEX JSON, or SARIF.

If you pass only a system name such as `reqpack audit npm`, ReqPack audits the packages currently reported as installed by that system.

### Basic Usage

```bash
# Audit explicit packages and print findings as a table
reqpack audit maven org.junit:junit:4.13 pip flask

# Audit currently installed packages for one system
reqpack audit npm

# Scoped syntax
reqpack audit maven:org.junit:junit:4.13 pip:flask

# Audit a project manifest
reqpack audit .
reqpack audit ./reqpack.lua
```

### Output Formats

| Flag | Value | Description |
|------|-------|-------------|
| `--format` | `table` | Terminal-friendly findings table *(default without `--output`)* |
| `--format` | `json` | ReqPack-native JSON |
| `--format` | `cyclonedx-vex-json` | CycloneDX JSON with vulnerability entries *(default for most file exports)* |
| `--format` | `sarif` | SARIF 2.1.0 export |

### Writing to a File

```bash
# Default file export: CycloneDX VEX JSON
reqpack audit pip flask --output reports/audit.json

# SARIF export for CI tools
reqpack audit maven org.junit:junit:4.13 --output reports/audit.sarif

# Explicit format
reqpack audit ./reqpack.lua --format cyclonedx-vex-json --output reports/audit.json
```

### Table Layout

```bash
# Wider table layout for terminal output
reqpack audit --wide

# Keep each row on one line and let terminal handle overflow
reqpack audit --no-wrap
```

### Exit Codes

- Without `--output`: exit `0` when no findings exist, `1` when findings are present.
- With `--output`: exit `0` when export succeeds, even if findings are present.

---

## 🔧 Architecture Overview

```plaintext
ReqPack (C++ Core)
├── CLI → Dispatcher → Lua Engine
├── Downloader → fetches registry & Lua scripts
├── LMDB storage → registry, history, transactions, vulnerability data
└── Lua Plugins → plugins/dnf/dnf.lua, plugins/maven/maven.lua, ...
```

---

## 📃 Registry System

### Format

The registry defines available languages and where to fetch their handlers:

```json
{
  "version": 102,
  "sources": {
    "python": "https://example.org/scripts/python.lua",
    "node": "https://example.org/scripts/node.lua",
    "java": "https://example.org/scripts/java.lua"
  }
}
```

### Sync Mechanism

* Registry metadata is stored under the ReqPack data directory
* Registry sources are loaded from the configured registry path (`registry.lua` or a registry directory)
* Downloaded plugin bundles are stored under the ReqPack data directory

---

## 🧰 Lua Script Example

```lua
-- plugins/python/python.lua

function install(package)
    if not is_installed("python3") then
        run_command("sudo apt install -y python3")
    end
    if not is_installed("pip3") then
        run_command("sudo apt install -y python3-pip")
    end
    run_command("pip3 install " .. package)
end
```

---

## 📊 Requirements

* C++17 or higher
* Lua (via sol2 binding)
* LMDB
* curl (for downloads)

---

## 📃 Build & Run

```bash
git clone https://github.com/Coditary/ReqPack.git
cd reqpack
mkdir build && cd build
cmake ..
make
```

---

## 🔁 CI & Releases

- `.github/workflows/ci.yml` runs on `push` and `pull_request`
- validated targets: `x86_64-linux`, `x86_64-darwin`, `aarch64-linux`, `aarch64-darwin`
- each target builds `ReqPack`, `core_unit_tests`, `core_integration_tests`, `exec_rules_unit_tests`, and `exec_rules_integration_tests`, then runs `ctest --test-dir build --output-on-failure`
- workflows reuse safe caches for CMake `FetchContent`, sol2 headers, `ccache`, and Homebrew downloads
- tags matching `v*` trigger `.github/workflows/release.yml`
- release assets are named `rqp-<tag>-<target>.tar.gz` and include shipped binary `rqp`
- each release also publishes `SHA256SUMS`

---

## ❤️ Contributing

Want to add support for a new package manager? Add a Lua plugin under `plugins/<system>/`, register a source in `registry.lua`, or submit a pull request to update the shared registry sources.

---

## 📄 License

MIT License — free to use, modify, and distribute.
