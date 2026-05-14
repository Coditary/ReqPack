#include "planner_internal.h"

#include "core/planning/request_resolution.h"

std::vector<Request> Planner::expandProxies(const std::vector<Request>& requests) const {
    return planner_expand_proxies(requests, this->config.planner.systemAliases);
}

std::optional<std::vector<Request>> Planner::resolveRequests(const std::vector<Request>& requests, std::string* errorMessage) const {
    RequestResolutionService resolver(this->registry, this->config);
    return resolver.resolveRequests(requests, errorMessage);
}

void Planner::ensurePluginsAvailable(const std::vector<Request>& requests) const {
    if (!this->config.planner.autoDownloadMissingPlugins) {
        return;
    }

    for (const Request& request : requests) {
        if (this->pluginExists(request.system)) {
            continue;
        }

        this->queuePluginDownload(request.system);
    }
}

bool Planner::pluginExists(const std::string& system) const {
    if (this->gatewayExists(system)) {
        return true;
    }
    return this->registry->getPlugin(system) != nullptr;
}

bool Planner::gatewayExists(const std::string& system) const {
    return this->securityGateway.isGatewaySystem(system);
}

void Planner::queuePluginDownload(const std::string& system) const {
    if (!this->downloader.downloadPlugin(system)) {
        return;
    }

    this->registry->scanDirectory(this->config.registry.pluginDirectory);
}
