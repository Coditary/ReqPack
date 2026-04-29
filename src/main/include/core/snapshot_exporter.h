#pragma once

#include "core/configuration.h"
#include "core/history_manager.h"
#include "core/types.h"

#include <string>
#include <vector>

// Generates a reqpack.lua manifest from the installed-packages snapshot
// tracked by HistoryManager.
class SnapshotExporter {
    ReqPackConfig config;

    std::string resolveOutputPath(const Request& request) const;
    std::string render(const std::vector<InstalledEntry>& entries) const;

public:
    explicit SnapshotExporter(const ReqPackConfig& config = DEFAULT_REQPACK_CONFIG);

    // Read history, render reqpack.lua, write to file or stdout.
    // Returns true on success.
    bool exportSnapshot(const Request& request) const;
};
