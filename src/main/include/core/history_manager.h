#pragma once

#include "core/configuration.h"
#include "core/types.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// One entry in the append-only history log.
struct HistoryEntry {
    std::string timestamp;      // ISO-8601 UTC, e.g. "2026-04-28T12:00:00Z"
    std::string action;         // "install" | "remove" | "update" | "ensure"
    std::string packageName;
    std::string packageVersion;
    std::string system;         // plugin/manager name, e.g. "dnf", "maven"
    std::string status;         // "success" | "failed"
    std::string errorMessage;
};

// One entry in the installed-packages snapshot.
struct InstalledEntry {
    std::string name;
    std::string version;
    std::string system;
    std::string installedAt;    // ISO-8601 UTC timestamp of last install/update
    std::string installMethod;  // "explicit" | "dependency" | "explicit+dependency" | "unknown"
    std::vector<std::string> owners;
};

inline std::string installed_owner_token(
    const std::string& prefix,
    const std::string& system,
    const std::string& name
) {
    return prefix + '\n' + system + '\n' + name;
}

inline std::string installed_package_owner_id(const std::string& system, const std::string& name, const std::string& version = {}) {
    (void)version;
    return installed_owner_token("pkg", system, name);
}

inline std::string installed_package_owner_id(const Package& package) {
    return installed_package_owner_id(package.system, package.name, package.version);
}

inline std::string installed_package_owner_id(const InstalledEntry& entry) {
    return installed_package_owner_id(entry.system, entry.name, entry.version);
}

inline std::string installed_root_owner_id(const std::string& system, const std::string& name, const std::string& version = {}) {
    (void)version;
    return installed_owner_token("root", system, name);
}

inline std::string installed_root_owner_id(const Package& package) {
    return installed_root_owner_id(package.system, package.name, package.version);
}

inline std::string installed_root_owner_id(const InstalledEntry& entry) {
    return installed_root_owner_id(entry.system, entry.name, entry.version);
}

// Manages XDG data history.jsonl (append-only event log, guarded by history.enabled)
// and an LMDB-backed installed-state snapshot under XDG data history/ (guarded by history.trackInstalled).
// Legacy installed.json in configured history directory is imported once on first access when present.
class HistoryManager {
    ReqPackConfig config;
    mutable std::mutex mutex;

    std::filesystem::path historyDir() const;
    std::filesystem::path historyLogPath() const;
    std::filesystem::path legacyInstalledStatePath() const;
    std::filesystem::path installedStateDatabasePath() const;

    bool ensureDirectory() const;

    // Low-level JSON helpers (no external dependency – hand-rolled minimal impl).
    static std::string escapeJson(const std::string& s);
    static std::string entryToJsonLine(const HistoryEntry& e);

    // Trim history.jsonl to respect maxLines / maxSizeMb limits.
    // Keeps the newest entries.  No-op when both limits are 0.
    void trimHistoryLog() const;

public:
    explicit HistoryManager(const ReqPackConfig& config = default_reqpack_config());

    // Read the current installed-packages snapshot from installed-state storage.
    std::vector<InstalledEntry> loadInstalledState() const;

    // Append one event to history.jsonl (only when history.enabled).
    bool appendEvent(const HistoryEntry& entry) const;

    // Update installed-state snapshot (only when history.trackInstalled).
    bool updateInstalledState(const HistoryEntry& entry) const;

    // Replace one system snapshot with authoritative installed-state data.
    bool replaceInstalledState(const std::string& system, const std::vector<InstalledEntry>& entries) const;

    // Attach direct/dependency ownership metadata to an already-installed package entry.
    bool mergeInstalledOwnership(const Package& package, const std::vector<std::string>& ownerIds, bool directRequest) const;

    // Remove ownership metadata from an already-installed package entry.
    bool subtractInstalledOwnership(const Package& package, const std::vector<std::string>& ownerIds) const;

    // Convenience: fill timestamp, then call appendEvent + updateInstalledState
    // according to the respective config flags.
    bool record(const HistoryEntry& entry) const;
};
