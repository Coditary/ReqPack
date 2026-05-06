#include "core/request_resolution.h"

#include "core/host_info.h"

#include <algorithm>
#include <cctype>

namespace {

constexpr std::size_t MAX_PROXY_RESOLUTION_DEPTH = 4;

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string item_id_for_request(const Request& request) {
    if (request.usesLocalTarget) {
        return request.system + ":local";
    }
    if (request.packages.size() == 1) {
        return request.system + ":" + request.packages.front();
    }
    return {};
}

}  // namespace

RequestResolutionService::RequestResolutionService(Registry* registry, const ReqPackConfig& config)
    : registry(registry), config(config) {}

PluginCallContext RequestResolutionService::buildProxyContext(IPlugin* plugin, const Request& request) const {
    if (plugin == nullptr) {
        return {};
    }

    return PluginCallContext{
        .pluginId = plugin->getPluginId(),
        .pluginDirectory = plugin->getPluginDirectory(),
        .scriptPath = plugin->getScriptPath(),
        .bootstrapPath = plugin->getBootstrapPath(),
        .flags = request.flags,
        .host = plugin->getRuntimeHost(),
        .proxy = proxy_config_for_system(this->config, plugin->getPluginId()),
        .currentItemId = item_id_for_request(request),
        .repositories = repositories_for_ecosystem(this->config, plugin->getPluginId()),
        .hostInfo = HostInfoService::currentSnapshot(),
    };
}

std::optional<Request> RequestResolutionService::resolveRequest(const Request& request, std::string* errorMessage) const {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    if (this->registry == nullptr || !this->config.planner.enableProxyExpansion || request.system.empty()) {
        return request;
    }

    std::vector<std::string> visitedSystems;
    return this->resolveRequestRecursive(request, visitedSystems, 0, errorMessage);
}

std::optional<std::vector<Request>> RequestResolutionService::resolveRequests(const std::vector<Request>& requests, std::string* errorMessage) const {
    std::vector<Request> resolvedRequests;
    resolvedRequests.reserve(requests.size());

    for (const Request& request : requests) {
        const std::optional<Request> resolved = this->resolveRequest(request, errorMessage);
        if (!resolved.has_value()) {
            return std::nullopt;
        }
        resolvedRequests.push_back(resolved.value());
    }

    return resolvedRequests;
}

std::optional<Request> RequestResolutionService::resolveRequestRecursive(
    const Request& request,
    std::vector<std::string>& visitedSystems,
    std::size_t depth,
    std::string* errorMessage
) const {
    Request current = request;
    current.system = this->registry->resolvePluginName(current.system);
    if (current.system.empty()) {
        return current;
    }

    if (depth >= MAX_PROXY_RESOLUTION_DEPTH) {
        if (errorMessage != nullptr) {
            *errorMessage = "proxy resolution depth exceeded for '" + current.system + "'";
        }
        return std::nullopt;
    }

    if (!this->registry->loadPlugin(current.system)) {
        return current;
    }

    IPlugin* plugin = this->registry->getPlugin(current.system);
    if (plugin == nullptr || !plugin->supportsProxyResolution()) {
        return current;
    }

    if (std::find(visitedSystems.begin(), visitedSystems.end(), current.system) != visitedSystems.end()) {
        if (errorMessage != nullptr) {
            *errorMessage = "proxy resolution cycle detected";
        }
        return std::nullopt;
    }

    visitedSystems.push_back(current.system);
    const PluginCallContext context = this->buildProxyContext(plugin, current);
    const std::optional<ProxyResolution> resolution = plugin->resolveProxyRequest(context, current);
    if (!resolution.has_value()) {
        if (errorMessage != nullptr && errorMessage->empty()) {
            *errorMessage = "proxy '" + current.system + "' could not resolve request";
        }
        visitedSystems.pop_back();
        return std::nullopt;
    }

    const std::string resolvedTarget = this->registry->resolvePluginName(to_lower_copy(resolution->targetSystem));
    if (resolvedTarget.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "proxy '" + current.system + "' resolved to empty target";
        }
        visitedSystems.pop_back();
        return std::nullopt;
    }

    if (resolvedTarget == current.system) {
        if (errorMessage != nullptr) {
            *errorMessage = "proxy '" + current.system + "' resolved to itself";
        }
        visitedSystems.pop_back();
        return std::nullopt;
    }

    if (const std::optional<ProxyConfig> proxyConfig = proxy_config_for_system(this->config, current.system); proxyConfig.has_value() &&
        !proxyConfig->targets.empty()) {
        const auto targetIt = std::find(proxyConfig->targets.begin(), proxyConfig->targets.end(), resolvedTarget);
        if (targetIt == proxyConfig->targets.end()) {
            if (errorMessage != nullptr) {
                *errorMessage = "proxy '" + current.system + "' resolved to unknown target '" + resolvedTarget + "'";
            }
            visitedSystems.pop_back();
            return std::nullopt;
        }
    }

    if (resolution->packages.has_value() && resolution->localPath.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = "proxy '" + current.system + "' returned both packages and localPath";
        }
        visitedSystems.pop_back();
        return std::nullopt;
    }

    Request next = current;
    next.system = resolvedTarget;
    if (resolution->flags.has_value()) {
        next.flags = resolution->flags.value();
    }
    if (resolution->localPath.has_value()) {
        next.localPath = resolution->localPath.value();
        next.usesLocalTarget = true;
        next.packages.clear();
    } else if (resolution->packages.has_value()) {
        next.packages = resolution->packages.value();
        next.localPath.clear();
        next.usesLocalTarget = false;
    }

    const std::optional<Request> resolved = this->resolveRequestRecursive(next, visitedSystems, depth + 1, errorMessage);
    visitedSystems.pop_back();
    return resolved;
}
