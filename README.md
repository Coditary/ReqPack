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

---

## 🍞 Example Usage

```bash
reqpack install pip flask
reqpack install npm express
reqpack install java lombok
```

ReqPack will detect whether Python or `pip` are installed, download them if missing, then proceed with the installation using its Lua plugin.

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
| `--format` | `table` | Tab-separated text table |

### Writing to a File

```bash
# Write CycloneDX JSON to a file (format inferred automatically)
reqpack sbom maven org.junit:junit --output bom.json

# Explicit format + output path
reqpack sbom pip flask --format cyclonedx-json --output reports/sbom.json
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

## 🔧 Architecture Overview

```plaintext
ReqPack (C++ Core)
├── CLI → Dispatcher → Lua Engine
├── Downloader → fetches registry & Lua scripts
├── SQLite Registry: remote.sqlite + local.sqlite
└── Lua Scripts: scripts/python.lua, scripts/node.lua, ...
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

* The remote registry is stored in `remote.sqlite`
* Custom extensions go into `local.sqlite`
* Updates use patch files (`patches/103.json`) containing minimal diffs

---

## 🧰 Lua Script Example

```lua
-- scripts/python.lua

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
* SQLite3
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

## ❤️ Contributing

Want to add support for a new package manager? Just drop a Lua script into the `scripts/` folder, register it in your `local.sqlite`, or submit a pull request to update the central registry.

---

## 📄 License

MIT License — free to use, modify, and distribute.
