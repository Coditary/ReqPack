#pragma once

#include "core/configuration.h"
#include "core/registry.h"
#include "core/types.h"

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
    RequestResolutionService(Registry* registry, const ReqPackConfig& config = DEFAULT_REQPACK_CONFIG);

    std::optional<Request> resolveRequest(const Request& request, std::string* errorMessage = nullptr) const;
    std::optional<std::vector<Request>> resolveRequests(const std::vector<Request>& requests, std::string* errorMessage = nullptr) const;
};
