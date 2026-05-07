#pragma once

#include "core/config/configuration.h"
#include "core/registry/registry.h"
#include "core/common/types.h"

#include <optional>
#include <string>
#include <vector>

class RequestResolutionService {
    Registry* registry;
    ReqPackConfig config;

    PluginCallContext buildProxyContext(IPlugin* plugin, const Request& request) const;
    std::optional<Request> resolveRequestRecursive(
        const Request& request,
        std::vector<std::string>& visitedSystems,
        std::size_t depth,
        std::string* errorMessage
    ) const;

public:
    RequestResolutionService(Registry* registry, const ReqPackConfig& config = default_reqpack_config());

    std::optional<Request> resolveRequest(const Request& request, std::string* errorMessage = nullptr) const;
    std::optional<std::vector<Request>> resolveRequests(const std::vector<Request>& requests, std::string* errorMessage = nullptr) const;
};
