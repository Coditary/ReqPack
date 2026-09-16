#include "cli_system_discovery.h"

#include "cli/cli.h"
#include "cli_parse_shared.h"
#include "core/plugins/plugin_bundle.h"
#include "core/registry/registry_database_core.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <vector>

namespace {

bool is_non_builtin_plugin_name(const std::string& name) {
    return !name.empty() && name != "rqp" && name != "sys";
}

void collect_plugin_bundle_systems(const std::filesystem::path& directory, std::set<std::string>& systems) {
    if (!std::filesystem::exists(directory)) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_directory()) {
            continue;
        }

        if (const auto layout = plugin_bundle_read_directory(entry.path()); layout.has_value()) {
            systems.insert(cli_internal::to_lower_copy(layout->metadata.name));
        }
    }
}

void append_enabled_gateways(const ReqPackConfig& config, std::set<std::string>& systems) {
    for (const auto& [name, gateway] : config.security.gateways) {
        if (gateway.enabled) {
            systems.insert(cli_internal::to_lower_copy(name));
        }
    }
}

void append_system_aliases(const ReqPackConfig& config, std::set<std::string>& systems) {
    for (const auto& [alias, target] : config.planner.systemAliases) {
        systems.insert(cli_internal::to_lower_copy(alias));
        systems.insert(cli_internal::to_lower_copy(target));
    }
}

std::set<std::string> collect_installed_plugin_names(const ReqPackConfig& config) {
    std::set<std::string> systems;
    collect_plugin_bundle_systems(config.registry.pluginDirectory, systems);

    std::set<std::string> filtered;
    for (const std::string& name : systems) {
        if (is_non_builtin_plugin_name(name)) {
            filtered.insert(name);
        }
    }
    return filtered;
}

std::vector<std::string> order_plugins_by_dependencies(const ReqPackConfig& config, const std::set<std::string>& plugins) {
    if (plugins.empty()) {
        return {};
    }

    std::map<std::string, int> inDegree;
    std::map<std::string, std::set<std::string>> dependents;
    for (const std::string& plugin : plugins) {
        inDegree.emplace(plugin, 0);
    }

    for (const std::string& plugin : plugins) {
        const std::optional<PluginBundleLayout> layout = plugin_bundle_find_installed(config, plugin);
        if (!layout.has_value()) {
            continue;
        }

        for (const Package& dependency : plugin_bundle_dependency_packages(layout.value())) {
            const std::string dependencyPlugin = cli_internal::to_lower_copy(dependency.system);
            if (!is_non_builtin_plugin_name(dependencyPlugin) || !plugins.contains(dependencyPlugin) || dependencyPlugin == plugin) {
                continue;
            }

            if (!dependents[dependencyPlugin].contains(plugin)) {
                dependents[dependencyPlugin].insert(plugin);
                ++inDegree[plugin];
            }
        }
    }

    std::vector<std::string> ordered;
    std::set<std::string> ready;
    for (const auto& [plugin, degree] : inDegree) {
        if (degree == 0) {
            ready.insert(plugin);
        }
    }

    while (!ready.empty()) {
        const std::string next = *ready.begin();
        ready.erase(next);
        ordered.push_back(next);

        for (const std::string& dependent : dependents[next]) {
            if (--inDegree[dependent] == 0) {
                ready.insert(dependent);
            }
        }
    }

    if (ordered.size() < plugins.size()) {
        std::vector<std::string> remaining;
        for (const std::string& plugin : plugins) {
            if (std::find(ordered.begin(), ordered.end(), plugin) == ordered.end()) {
                remaining.push_back(plugin);
            }
        }
        std::sort(remaining.begin(), remaining.end());
        ordered.insert(ordered.end(), remaining.begin(), remaining.end());
    }

    return ordered;
}

}  // namespace

namespace cli_internal {

std::set<std::string> discover_primary_systems(const ReqPackConfig& config) {
    std::set<std::string> systems;
    systems.insert("rqp");

    RegistryDatabase registryDatabase(config);
    if (registryDatabase.ensureReady()) {
        for (const RegistryRecord& record : registryDatabase.getAllRecords()) {
            if (!record.alias && !registry_record_is_package_entry(record)) {
                systems.insert(to_lower_copy(record.name));
            }
        }
    }

    append_enabled_gateways(config, systems);
    collect_plugin_bundle_systems(config.registry.pluginDirectory, systems);

    for (const auto& [_, target] : config.planner.systemAliases) {
        systems.insert(to_lower_copy(target));
    }

    return systems;
}

std::vector<std::string> discover_installed_plugins(const ReqPackConfig& config) {
    return order_plugins_by_dependencies(config, collect_installed_plugin_names(config));
}

std::set<std::string> discover_systems(const ReqPackConfig& config) {
    std::set<std::string> systems;
    systems.insert("rqp");

    RegistryDatabase registryDatabase(config);
    if (registryDatabase.ensureReady()) {
        for (const RegistryRecord& record : registryDatabase.getAllRecords()) {
            systems.insert(to_lower_copy(record.name));
            if (record.alias && !record.source.empty()) {
                systems.insert(to_lower_copy(record.source));
            }
        }
    }

    if (!std::filesystem::exists(config.registry.pluginDirectory)) {
        append_enabled_gateways(config, systems);
        append_system_aliases(config, systems);
        return systems;
    }

    collect_plugin_bundle_systems(config.registry.pluginDirectory, systems);
    append_system_aliases(config, systems);
    append_enabled_gateways(config, systems);
    return systems;
}

}  // namespace cli_internal

std::set<std::string> Cli::discover_primary_systems(const ReqPackConfig& config) {
    return cli_internal::discover_primary_systems(config);
}

std::vector<std::string> Cli::discover_installed_plugins(const ReqPackConfig& config) {
    return cli_internal::discover_installed_plugins(config);
}

std::set<std::string> Cli::discover_systems(const ReqPackConfig& config) {
    return cli_internal::discover_systems(config);
}
