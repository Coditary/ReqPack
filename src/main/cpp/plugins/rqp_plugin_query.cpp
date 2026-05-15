#include "rqp_plugin_internal.h"

#include "core/plugins/plugin_bundle.h"
#include "core/registry/registry_database.h"
#include "core/registry/registry_database_core.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool rq_package_installed(const std::filesystem::path& stateRoot, const Package& package) {
    const std::filesystem::path packageRoot = stateRoot / package.name;
    std::error_code error;
    if (!std::filesystem::is_directory(packageRoot, error)) {
        return false;
    }

    const std::string prefix = package.version.empty() ? std::string{} : package.name + "@" + package.version + "-";
    for (const auto& entry : std::filesystem::directory_iterator(packageRoot, error)) {
        if (error || !entry.is_directory()) {
            continue;
        }
        if (prefix.empty()) {
            return true;
        }
        if (entry.path().filename().string().rfind(prefix, 0) == 0) {
            return true;
        }
    }

    return false;
}

void collect_materialized_plugin_scripts(
    const std::filesystem::path& root,
    std::map<std::string, std::filesystem::path>& scriptPaths
) {
    std::error_code error;
    if (!std::filesystem::exists(root, error) || error) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error || !entry.is_directory()) {
            continue;
        }

        const std::optional<PluginBundleLayout> layout = plugin_bundle_read_directory(entry.path());
        if (!layout.has_value()) {
            continue;
        }

        const std::string pluginId = to_lower_copy(layout->metadata.name);
        if (pluginId == BUILTIN_RQP_PLUGIN_ID) {
            continue;
        }

        scriptPaths[pluginId] = layout->runScriptPath;
    }
}

std::map<std::string, std::filesystem::path> installed_plugin_script_paths(const ReqPackConfig& config) {
    std::map<std::string, std::filesystem::path> scriptPaths;
    collect_materialized_plugin_scripts(std::filesystem::path(config.registry.pluginDirectory), scriptPaths);

    std::error_code error;
    const std::filesystem::path workspacePluginDirectory = std::filesystem::current_path(error) / "plugins";
    const std::filesystem::path configuredPluginDirectory = std::filesystem::path(config.registry.pluginDirectory);
    if (!error && workspacePluginDirectory != configuredPluginDirectory) {
        collect_materialized_plugin_scripts(workspacePluginDirectory, scriptPaths);
    }

    return scriptPaths;
}

std::string try_read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string plugin_version_from_script_text(const std::string& script) {
    static const std::regex versionPattern(R"(function\s+plugin\.getVersion\s*\(\s*\)\s*return\s*["']([^"']+)["'])");

    if (script.empty()) {
        return {};
    }

    std::smatch match;
    if (!std::regex_search(script, match, versionPattern) || match.size() < 2) {
        return {};
    }

    return match[1].str();
}

std::string plugin_version_from_script(const std::filesystem::path& scriptPath) {
    if (const std::optional<PluginBundleLayout> layout = plugin_bundle_read_directory(scriptPath.parent_path()); layout.has_value()) {
        return layout->metadata.version;
    }

    return plugin_version_from_script_text(try_read_text_file(scriptPath));
}

PackageInfo installed_plugin_info(
    const std::string& pluginId,
    const RegistryRecord* record,
    const std::string& version,
    const std::string& fallbackType,
    const std::string& fallbackDescription
) {
    PackageInfo info;
    info.system = BUILTIN_RQP_PLUGIN_ID;
    info.name = pluginId;
    info.packageId = pluginId;
    info.version = version;
    info.status = "installed";
    info.installed = "true";
    info.packageType = fallbackType;

    std::string description = fallbackDescription;
    if (record != nullptr) {
        if (record->alias) {
            info.packageType = "alias";
        } else if (!record->role.empty()) {
            info.packageType = record->role;
        }

        if (!record->description.empty()) {
            description = record->description;
        }

        info.sourceUrl = record->source;
    }

    info.summary = description;
    info.description = description;
    return info;
}

std::vector<std::string> search_terms_from_prompt(const std::string& prompt) {
    std::vector<std::string> terms;
    std::string current;

    for (const char character : prompt) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            if (!current.empty()) {
                terms.push_back(std::move(current));
                current.clear();
            }
            continue;
        }

        current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }

    if (!current.empty()) {
        terms.push_back(std::move(current));
    }

    return terms;
}

bool package_matches_search_terms(const PackageInfo& info, const std::vector<std::string>& terms) {
    if (terms.empty()) {
        return true;
    }

    std::string haystack;
    const auto append = [&](const std::string& value) {
        if (value.empty()) {
            return;
        }
        if (!haystack.empty()) {
            haystack.push_back('\n');
        }
        haystack += to_lower_copy(value);
    };

    append(info.name);
    append(info.packageId);
    append(info.version);
    append(info.summary);
    append(info.description);
    append(info.sourceUrl);
    append(info.packageType);

    return std::all_of(terms.begin(), terms.end(), [&](const std::string& term) {
        return haystack.find(term) != std::string::npos;
    });
}

bool is_installed_rqp_package_search_item(const PackageInfo& info) {
    return info.system == BUILTIN_RQP_PLUGIN_ID && info.installed == "true" && info.packageType.empty();
}

std::string joined_systems(const std::vector<std::string>& systems) {
    return rq_join_systems(systems);
}

std::pair<std::string, std::string> split_package_name_version(std::string value) {
    const std::size_t versionSeparator = value.rfind('@');
    if (versionSeparator == std::string::npos || versionSeparator == 0 || versionSeparator + 1 >= value.size()) {
        return {std::move(value), {}};
    }
    return {value.substr(0, versionSeparator), value.substr(versionSeparator + 1)};
}

std::string version_from_registry_record(const RegistryRecord& record) {
    if (registry_record_is_package_entry(record)) {
        return {};
    }
    if (!record.bundlePath.empty()) {
        if (const std::optional<PluginBundleLayout> layout = plugin_bundle_find_root(record.bundlePath, record.name); layout.has_value()) {
            return layout->metadata.version;
        }
    }

    std::error_code error;
    const std::filesystem::path sourcePath(record.source);
    if (std::filesystem::exists(sourcePath, error) && !error && std::filesystem::is_directory(sourcePath, error) && !error) {
        if (const std::optional<PluginBundleLayout> layout = plugin_bundle_find_root(sourcePath, record.name); layout.has_value()) {
            return layout->metadata.version;
        }
    }

    return plugin_version_from_script_text(record.script);
}

PackageInfo rq_repository_info_item(const RqRepositoryPackage& package, bool installed) {
    PackageInfo info;
    info.system = BUILTIN_RQP_PLUGIN_ID;
    info.name = package.name;
    info.packageId = package.name;
    info.version = package.version + "-" + std::to_string(package.release) + "+r" + std::to_string(package.revision);
    info.status = installed ? "installed" : "available";
    info.installed = installed ? "true" : "false";
    info.summary = package.summary;
    info.description = package.summary;
    info.packageType = "package";
    info.architecture = package.architecture;
    info.targetSystems = joined_systems(package.systems);
    info.sourceUrl = package.url;
    info.repository = package.repository;
    info.tags = package.tags;
    return info;
}

PackageInfo registry_plugin_info(const RegistryRecord& record) {
    PackageInfo info;
    info.system = BUILTIN_RQP_PLUGIN_ID;
    info.name = record.name;
    info.packageId = record.name;
    info.version = version_from_registry_record(record);
    info.packageType = record.alias ? "alias" : (record.role.empty() ? "plugin" : record.role);
    info.sourceUrl = record.source;

    if (record.alias && !record.source.empty()) {
        info.summary = record.description.empty() ? ("Alias for " + record.source) : record.description;
    } else if (registry_record_is_package_entry(record)) {
        info.summary = record.description.empty() ? "Registry package" : record.description;
    } else {
        info.summary = record.description.empty() ? "Registry plugin" : record.description;
    }
    info.description = info.summary;
    return info;
}

PackageInfo rq_info_item(const std::string& name, const std::string& version, const std::string& description) {
    PackageInfo info;
    info.name = name;
    info.version = version;
    info.summary = description;
    info.description = description;
    return info;
}

}  // namespace

std::vector<Package> RqpPlugin::getMissingPackages(const std::vector<Package>& packages) {
    std::vector<Package> missingPackages;
    const std::filesystem::path stateRoot(config_.rqp.statePath);
    for (const Package& package : packages) {
        if (!rq_package_installed(stateRoot, package)) {
            missingPackages.push_back(package);
        }
    }
    return missingPackages;
}

std::vector<PackageInfo> RqpPlugin::list(const PluginCallContext& context) {
    (void)context;
    std::vector<PackageInfo> packages;
    std::set<std::string> emittedNames;
    std::map<std::string, std::string> installedVersions;
    std::map<std::string, RegistryRecord> recordsByName;
    std::vector<RegistryRecord> aliasRecords;
    RqpStateStore stateStore(config_);

    RegistryDatabase registryDatabase(config_);
    if (registryDatabase.ensureReady()) {
        for (const RegistryRecord& record : registryDatabase.getAllRecords()) {
            const std::string normalizedName = to_lower_copy(record.name);
            if (normalizedName.empty()) {
                continue;
            }
            if (record.alias) {
                aliasRecords.push_back(record);
                continue;
            }
            recordsByName[normalizedName] = record;
        }
    }

    const auto appendPackage = [&](PackageInfo info) {
        const std::string normalizedName = to_lower_copy(info.name);
        if (normalizedName.empty() || !emittedNames.insert(normalizedName).second) {
            return;
        }
        packages.push_back(std::move(info));
    };

    const RegistryRecord* builtinRecord = nullptr;
    if (const auto builtinIt = recordsByName.find(BUILTIN_RQP_PLUGIN_ID); builtinIt != recordsByName.end()) {
        builtinRecord = &builtinIt->second;
    }

    const std::string builtinVersion = this->getVersion();
    installedVersions[BUILTIN_RQP_PLUGIN_ID] = builtinVersion;
    appendPackage(installed_plugin_info(
        BUILTIN_RQP_PLUGIN_ID,
        builtinRecord,
        builtinVersion,
        "builtin",
        this->getName()
    ));

    for (const RqpInstalledPackage& installed : stateStore.listInstalled()) {
        appendPackage(packageInfoFromInstalled(installed));
    }

    for (const auto& [pluginId, scriptPath] : installed_plugin_script_paths(config_)) {
        const RegistryRecord* record = nullptr;
        if (const auto it = recordsByName.find(pluginId); it != recordsByName.end()) {
            record = &it->second;
        }

        const std::string version = plugin_version_from_script(scriptPath);
        installedVersions[pluginId] = version;
        appendPackage(installed_plugin_info(
            pluginId,
            record,
            version,
            "plugin",
            "Locally installed plugin"
        ));
    }

    for (const RegistryRecord& record : aliasRecords) {
        const std::string normalizedAlias = to_lower_copy(record.name);
        const std::string normalizedTarget = to_lower_copy(record.source);
        if (normalizedAlias.empty() || normalizedTarget.empty()) {
            continue;
        }
        if (!installedVersions.contains(normalizedTarget)) {
            continue;
        }

        appendPackage(installed_plugin_info(
            normalizedAlias,
            &record,
            installedVersions[normalizedTarget],
            "alias",
            record.description.empty() ? "Alias for " + normalizedTarget : record.description
        ));
    }

    std::sort(packages.begin(), packages.end(), [](const PackageInfo& left, const PackageInfo& right) {
        return left.name < right.name;
    });

    for (PackageInfo& package : packages) {
        if (package.description.empty()) {
            package.description = package.summary;
        }
    }

    return packages;
}

std::vector<PackageInfo> RqpPlugin::outdated(const PluginCallContext& context) {
    (void)context;
    return {};
}

std::vector<PackageInfo> RqpPlugin::search(const PluginCallContext& context, const std::string& prompt) {
    std::vector<PackageInfo> results;
    std::set<std::string> emittedNames;
    const std::vector<std::string> terms = search_terms_from_prompt(prompt);

    for (PackageInfo info : this->list(context)) {
        if (is_installed_rqp_package_search_item(info) || !package_matches_search_terms(info, terms)) {
            continue;
        }

        const std::string normalizedName = to_lower_copy(info.name);
        if (normalizedName.empty() || !emittedNames.insert(normalizedName).second) {
            continue;
        }

        results.push_back(std::move(info));
    }

    RegistryDatabase registryDatabase(config_);
    if (registryDatabase.ensureReady()) {
        for (const RegistryRecord& record : registryDatabase.getAllRecords()) {
            if (registry_record_is_package_entry(record)) {
                continue;
            }
            const std::string normalizedName = to_lower_copy(record.name);
            if (normalizedName.empty() || emittedNames.contains(normalizedName)) {
                continue;
            }

            PackageInfo info = registry_plugin_info(record);
            if (!package_matches_search_terms(info, terms)) {
                continue;
            }

            emittedNames.insert(normalizedName);
            results.push_back(std::move(info));
        }
    }

    const std::vector<RqRepositoryIndex> indexes = loadRepositoryIndexes(context);
    const std::set<std::string> hostSystems = rq_host_system_tokens(*HostInfoService::currentSnapshot());
    for (const RqRepositoryIndex& index : indexes) {
        for (const RqRepositoryPackage& package : index.packages) {
            const std::string normalizedName = to_lower_copy(package.name);
            if (normalizedName.empty() || emittedNames.contains(normalizedName)) {
                continue;
            }
            if (!rq_architecture_matches(package.architecture, rq_host_architecture())) {
                continue;
            }
            if (!rq_system_matches(package.systems, hostSystems, rq_merged_system_aliases(config_))) {
                continue;
            }

            PackageInfo info = rq_repository_info_item(package, false);
            if (!package_matches_search_terms(info, terms)) {
                continue;
            }

            emittedNames.insert(normalizedName);
            results.push_back(std::move(info));
        }
    }

    std::sort(results.begin(), results.end(), [](const PackageInfo& left, const PackageInfo& right) {
        return left.name < right.name;
    });
    return results;
}

PackageInfo RqpPlugin::info(const PluginCallContext& context, const std::string& packageName) {
    const auto [resolvedName, requestedVersion] = split_package_name_version(packageName);
    std::vector<RqpInstalledPackage> matches = RqpStateStore(config_).findInstalled(resolvedName, requestedVersion);
    if (!matches.empty()) {
        if (matches.size() > 1) {
            return rq_info_item(resolvedName, {}, "multiple installed versions");
        }
        return packageInfoFromInstalled(matches.front());
    }

    const std::vector<RqRepositoryIndex> indexes = loadRepositoryIndexes(context);
    const std::optional<RqRepositoryPackage> candidate = rq_repository_resolve_package(
        indexes,
        resolvedName,
        requestedVersion,
        rq_host_architecture(),
        rq_host_system_tokens(*HostInfoService::currentSnapshot()),
        config_
    );
    if (candidate.has_value()) {
        return rq_repository_info_item(candidate.value(), false);
    }

    RegistryDatabase registryDatabase(config_);
    if (registryDatabase.ensureReady()) {
        if (const std::optional<RegistryRecord> record = registryDatabase.getRecord(resolvedName); record.has_value()) {
            return registry_plugin_info(record.value());
        }
    }
    return {};
}

std::optional<Package> RqpPlugin::resolvePackage(const PluginCallContext& context, const Package& package) {
    std::vector<RqpInstalledPackage> matches = RqpStateStore(config_).findInstalled(package.name);
    if (!matches.empty()) {
        std::sort(matches.begin(), matches.end(), compareInstalledVersions);
        Package resolved = package;
        resolved.version = installedVersionString(matches.back().metadata);
        return resolved;
    }

    const std::vector<RqRepositoryIndex> indexes = loadRepositoryIndexes(context);
    const std::optional<RqRepositoryPackage> candidate = rq_repository_resolve_package(
        indexes,
        package.name,
        package.version,
        rq_host_architecture(),
        rq_host_system_tokens(*HostInfoService::currentSnapshot()),
        config_
    );
    if (!candidate.has_value()) {
        return std::nullopt;
    }

    Package resolved = package;
    resolved.version = candidate->version + "-" + std::to_string(candidate->release) + "+r" + std::to_string(candidate->revision);
    return resolved;
}
