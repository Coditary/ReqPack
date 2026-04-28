#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

inline const std::string MANIFEST_FILENAME = "reqpack.lua";

/// A single package entry from a reqpack.lua manifest.
struct ManifestEntry {
    std::string system;
    std::string name;
    std::string version;  // optional
    std::vector<std::string> flags;  // optional per-entry flags
};

/// Loads and parses reqpack.lua manifest files.
///
/// Manifest format (reqpack.lua):
///
///   -- Option A: return a table
///   return {
///     packages = {
///       { system = "dnf",  name = "curl" },
///       { system = "dnf",  name = "git",  version = "2.x" },
///       { system = "npm",  name = "express", version = "4.18.0" },
///     }
///   }
///
///   -- Option B: assign global variable
///   packages = {
///     { system = "dnf", name = "curl" },
///     { system = "npm", name = "express" },
///   }
///
/// Each entry must have a "system" and "name" field.
/// "version" and "flags" are optional.
class ManifestLoader {
public:
    /// Loads and parses the given reqpack.lua file.
    /// @throws std::runtime_error if the file is missing, cannot be parsed,
    ///         or contains invalid entries.
    static std::vector<ManifestEntry> load(const std::filesystem::path& manifestPath);
};
