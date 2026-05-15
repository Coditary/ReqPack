#include "cli_system_discovery.h"

#include "cli/cli.h"
#include "cli_parse_shared.h"
#include "core/plugins/plugin_bundle.h"
#include "core/registry/registry_database_core.h"

#include <filesystem>

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

std::set<std::string> discover_non_builtin_plugins(const ReqPackConfig& config) {
    std::set<std::string> systems;
    const RegistrySourceMap configuredSources = collect_explicit_registry_sources(config);

    ReqPackConfig mainRegistryConfig = config;
    mainRegistryConfig.registry.sources.clear();
    mainRegistryConfig.downloader.pluginSources.clear();

    RegistryDatabase registryDatabase(mainRegistryConfig);
    const bool mainRegistryReady = registryDatabase.refreshMainRegistry();

    for (const auto& [name, entry] : configuredSources) {
        const std::string normalizedName = to_lower_copy(name);
        if (!entry.alias && is_non_builtin_plugin_name(normalizedName)) {
            systems.insert(normalizedName);
        }
    }

    if (std::filesystem::exists(config.registry.pluginDirectory)) {
        for (const auto& entry : std::filesystem::directory_iterator(config.registry.pluginDirectory)) {
            if (!entry.is_directory()) {
                continue;
            }

            const auto layout = plugin_bundle_read_directory(entry.path());
            if (!layout.has_value()) {
                continue;
            }

            const std::string name = to_lower_copy(layout->metadata.name);
            if (is_non_builtin_plugin_name(name)) {
                systems.insert(name);
            }
        }
    }

    if (systems.empty() && mainRegistryReady) {
        for (const RegistryRecord& record : registryDatabase.getAllRecords()) {
            const std::string name = to_lower_copy(record.name);
            if (!record.alias && !registry_record_is_package_entry(record) && is_non_builtin_plugin_name(name)) {
                systems.insert(name);
            }
        }
    }

    return systems;
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

std::set<std::string> Cli::discover_non_builtin_plugins(const ReqPackConfig& config) {
    return cli_internal::discover_non_builtin_plugins(config);
}

std::set<std::string> Cli::discover_systems(const ReqPackConfig& config) {
    return cli_internal::discover_systems(config);
}
