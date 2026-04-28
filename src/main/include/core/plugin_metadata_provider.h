#pragma once

#include "plugins/iplugin.h"

#include <optional>
#include <string>
#include <vector>

class PluginMetadataProvider {
public:
    virtual ~PluginMetadataProvider() = default;
    virtual std::optional<PluginSecurityMetadata> getPluginSecurityMetadata(const std::string& name) = 0;
    virtual std::vector<std::string> getKnownPluginNames() {
        return {};
    }
};
