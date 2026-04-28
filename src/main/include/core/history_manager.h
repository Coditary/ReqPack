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
};

// Manages ~/.reqpack/history/history.jsonl  (append-only event log, guarded by history.enabled)
// and      ~/.reqpack/history/installed.json (installed state snapshot, guarded by history.trackInstalled).
// Both files are controlled independently via HistoryConfig.
class HistoryManager {
    ReqPackConfig config;
    mutable std::mutex mutex;

    std::filesystem::path historyDir() const;
    std::filesystem::path historyLogPath() const;
    std::filesystem::path installedStatePath() const;

    bool ensureDirectory() const;

    // Low-level JSON helpers (no external dependency – hand-rolled minimal impl).
    static std::string escapeJson(const std::string& s);
    static std::string entryToJsonLine(const HistoryEntry& e);

    // Trim history.jsonl to respect maxLines / maxSizeMb limits.
    // Keeps the newest entries.  No-op when both limits are 0.
    void trimHistoryLog() const;

    // Installed-state helpers.
    bool saveInstalledState(const std::vector<InstalledEntry>& entries) const;

public:
    explicit HistoryManager(const ReqPackConfig& config = DEFAULT_REQPACK_CONFIG);

    // Read the current installed-packages snapshot from installed.json.
    std::vector<InstalledEntry> loadInstalledState() const;

    // Append one event to history.jsonl (only when history.enabled).
    bool appendEvent(const HistoryEntry& entry) const;

    // Update installed.json (only when history.trackInstalled).
    bool updateInstalledState(const HistoryEntry& entry) const;

    // Convenience: fill timestamp, then call appendEvent + updateInstalledState
    // according to the respective config flags.
    bool record(const HistoryEntry& entry) const;
};
