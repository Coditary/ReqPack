#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "plugins/iplugin.h"

class LuaBridgeExecutionPolicy {
public:
    static std::optional<std::string> validate(const PluginSecurityMetadata& metadata,
                                               const std::string& pluginId,
                                               const std::string& pluginDirectory,
                                               const std::string& command,
                                               const std::vector<std::filesystem::path>& runtimeWriteRoots = {});
};
