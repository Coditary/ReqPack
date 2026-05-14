#include "planner_internal.h"

#include "core/plugins/plugin_bundle.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

namespace planner_internal {

std::filesystem::path requirements_marker_path(const ReqPackConfig& config, const std::string& system) {
    return plugin_bundle_ready_marker_path(std::filesystem::path(config.registry.pluginDirectory) / system);
}

void set_internal_ensure_order_flag(Package& package, const std::size_t order) {
    const std::string prefix(INTERNAL_ENSURE_ORDER_FLAG_PREFIX);
    package.flags.erase(std::remove_if(package.flags.begin(), package.flags.end(), [&](const std::string& flag) {
        return flag.rfind(prefix, 0) == 0;
    }), package.flags.end());
    package.flags.push_back(prefix + std::to_string(order));
}

void propagate_internal_ensure_order_flag(const Package& source, Package& target) {
    const std::string prefix(INTERNAL_ENSURE_ORDER_FLAG_PREFIX);
    const auto it = std::find_if(source.flags.begin(), source.flags.end(), [&](const std::string& flag) {
        return flag.rfind(prefix, 0) == 0;
    });
    if (it == source.flags.end()) {
        return;
    }
    target.flags.erase(std::remove_if(target.flags.begin(), target.flags.end(), [&](const std::string& flag) {
        return flag.rfind(prefix, 0) == 0;
    }), target.flags.end());
    target.flags.push_back(*it);
}

Package resolve_dependency_system(Package dependency, const Registry* registry) {
    if (dependency.system.empty()) {
        return dependency;
    }

    dependency.system = registry->resolvePluginName(dependency.system);
    return dependency;
}

IPlugin* load_plugin_for_use(Registry* registry, const std::string& system) {
    if (registry == nullptr) {
        return nullptr;
    }
    IPlugin* plugin = registry->getPlugin(system);
    if (plugin == nullptr || !registry->loadPlugin(system)) {
        return nullptr;
    }
    return registry->getPlugin(system);
}

}  // namespace planner_internal

bool Planner::shouldInstallPluginDependencies(const std::string& system) const {
    const std::string resolvedSystem = this->registry->resolvePluginName(system);
    if (this->pluginRequirementsProvisioned(resolvedSystem)) {
        return false;
    }

    if (this->pluginRequirementsSatisfied(resolvedSystem)) {
        this->markPluginRequirementsProvisioned(resolvedSystem);
        return false;
    }

    return true;
}

bool Planner::pluginRequirementsSatisfied(const std::string& system) const {
    const std::optional<PluginBundleLayout> layout = plugin_bundle_find_installed(this->config, system);
    if (!layout.has_value()) {
        return false;
    }

    std::vector<Package> requirements;
    for (Package dependency : plugin_bundle_dependency_packages(layout.value())) {
        requirements.push_back(planner_internal::resolve_dependency_system(
            planner_normalize_dependency(std::move(dependency), system),
            this->registry
        ));
    }

    return this->filterMissingDependencies(requirements).empty();
}

bool Planner::pluginRequirementsProvisioned(const std::string& system) const {
    return std::filesystem::exists(planner_internal::requirements_marker_path(this->config, system));
}

void Planner::markPluginRequirementsProvisioned(const std::string& system) const {
    const std::filesystem::path markerPath = planner_internal::requirements_marker_path(this->config, system);
    std::error_code directoryError;
    std::filesystem::create_directories(markerPath.parent_path(), directoryError);
    if (directoryError) {
        return;
    }

    std::ofstream marker(markerPath, std::ios::binary | std::ios::trunc);
    if (!marker) {
        return;
    }

    marker << "ready\n";
}

std::vector<Request> Planner::filterRequestedPackages(const std::vector<Request>& requests) const {
    std::vector<Request> filteredRequests;
    filteredRequests.reserve(requests.size());

    for (const Request& request : requests) {
        if (request.action != ActionType::INSTALL || request.packages.empty()) {
            if (request.action == ActionType::INSTALL && request.usesLocalTarget) {
                filteredRequests.push_back(request);
                continue;
            }
            filteredRequests.push_back(request);
            continue;
        }

        if (this->gatewayExists(request.system)) {
            filteredRequests.push_back(request);
            continue;
        }
        IPlugin* plugin = planner_internal::load_plugin_for_use(this->registry, request.system);
        if (plugin == nullptr) {
            filteredRequests.push_back(request);
            continue;
        }

        std::vector<Package> requestedPackages;
        requestedPackages.reserve(request.packages.size());
        for (const std::string& packageSpecifier : request.packages) {
            requestedPackages.push_back(this->makeRequestedPackage(request, packageSpecifier));
        }

        const std::vector<Package> missingPackages = plugin->getMissingPackages(requestedPackages);
        if (missingPackages.empty()) {
            filteredRequests.push_back(request);
            continue;
        }
        const std::optional<Request> filteredRequest = planner_filter_install_request(request, missingPackages);
        if (!filteredRequest.has_value()) {
            continue;
        }

        filteredRequests.push_back(filteredRequest.value());
    }

    return filteredRequests;
}

std::vector<Package> Planner::collectPluginDependencies(const std::vector<Request>& requests) const {
    std::vector<Package> dependencies;

    for (std::size_t requestIndex = 0; requestIndex < requests.size(); ++requestIndex) {
        const Request& request = requests[requestIndex];
        if (this->gatewayExists(request.system)) {
            continue;
        }
        const std::optional<PluginBundleLayout> layout = plugin_bundle_find_installed(this->config, request.system);
        if (!layout.has_value()) {
            continue;
        }

        for (Package dependency : plugin_bundle_dependency_packages(layout.value())) {
            Package normalizedDependency = planner_internal::resolve_dependency_system(
                planner_normalize_dependency(std::move(dependency), request.system),
                this->registry
            );
            normalizedDependency.action = ActionType::ENSURE;
            planner_internal::set_internal_ensure_order_flag(normalizedDependency, requestIndex);
            dependencies.push_back(std::move(normalizedDependency));
        }
    }

    return dependencies;
}

std::vector<Package> Planner::filterMissingDependencies(const std::vector<Package>& dependencies) const {
    std::vector<Package> missingDependencies;
    std::map<std::string, std::vector<Package>> dependenciesBySystem;
    std::map<std::string, IPlugin*> pluginsBySystem;

    for (const Package& dependency : dependencies) {
        if (dependency.system.empty()) {
            missingDependencies.push_back(dependency);
            continue;
        }

        if (this->gatewayExists(dependency.system)) {
            missingDependencies.push_back(dependency);
            continue;
        }

        if (!this->dependencyExists(dependency) && this->config.planner.autoDownloadMissingDependencies) {
            this->queueDependencyDownload(dependency);
        }

        IPlugin* plugin = planner_internal::load_plugin_for_use(this->registry, dependency.system);
        if (plugin == nullptr) {
            missingDependencies.push_back(dependency);
            continue;
        }

        const std::string resolvedSystem = this->registry->resolvePluginName(dependency.system);
        dependenciesBySystem[resolvedSystem].push_back(dependency);
        pluginsBySystem[resolvedSystem] = plugin;
    }

    for (auto& [system, systemDependencies] : dependenciesBySystem) {
        IPlugin* plugin = pluginsBySystem[system];
        const std::vector<Package> pluginMissingDependencies = plugin->getMissingPackages(systemDependencies);
        missingDependencies.insert(missingDependencies.end(), pluginMissingDependencies.begin(), pluginMissingDependencies.end());
    }

    return missingDependencies;
}

void Planner::ensurePluginDependenciesAvailable(const std::vector<Package>& dependencies) const {
    if (!this->config.planner.autoDownloadMissingDependencies) {
        return;
    }

    for (const Package& dependency : dependencies) {
        if (this->dependencyExists(dependency)) {
            continue;
        }

        this->queueDependencyDownload(dependency);
    }
}

bool Planner::dependencyExists(const Package& dependency) const {
    if (!dependency.system.empty() && this->gatewayExists(dependency.system)) {
        return true;
    }
    return !dependency.system.empty() && this->registry->getPlugin(dependency.system) != nullptr;
}

void Planner::queueDependencyDownload(const Package& dependency) const {
    if (dependency.system.empty()) {
        return;
    }

    if (!this->downloader.downloadPlugin(dependency.system)) {
        return;
    }

    this->registry->scanDirectory(this->config.registry.pluginDirectory);
}
