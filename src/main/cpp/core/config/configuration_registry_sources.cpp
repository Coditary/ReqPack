#include "configuration_internal.h"

#include <sol/sol.hpp>

std::filesystem::path registry_database_directory(const std::filesystem::path& registryPath) {
    const std::filesystem::path resolvedPath = configuration_internal::expand_user_path(registryPath);
    if (resolvedPath.has_extension()) {
        return resolvedPath.parent_path();
    }

    return resolvedPath;
}

std::filesystem::path registry_source_file_path(const std::filesystem::path& registryPath) {
    const std::filesystem::path resolvedPath = configuration_internal::expand_user_path(registryPath);
    if (resolvedPath.has_extension()) {
        return resolvedPath;
    }

    return resolvedPath / "registry.lua";
}

RegistrySourceMap load_registry_sources_from_lua(const std::filesystem::path& sourcePath) {
    const std::filesystem::path resolvedSourcePath = configuration_internal::expand_user_path(sourcePath);
    if (!std::filesystem::exists(resolvedSourcePath)) {
        return {};
    }

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table, sol::lib::string, sol::lib::math);

    sol::load_result loadResult = lua.load_file(resolvedSourcePath.string());
    if (!loadResult.valid()) {
        return {};
    }

    const sol::protected_function_result executionResult = loadResult();
    if (!executionResult.valid()) {
        return {};
    }

    sol::table root;
    if (executionResult.get_type() == sol::type::table) {
        root = executionResult;
    } else {
        const sol::object registryObject = lua["registry"];
        if (registryObject.get_type() != sol::type::table) {
            return {};
        }

        root = registryObject.as<sol::table>();
    }

    const sol::optional<sol::table> sources = root["sources"];
    return configuration_internal::load_registry_sources_from_table(sources.has_value() ? sources.value() : root);
}

RegistrySourceMap collect_explicit_registry_sources(const ReqPackConfig& config) {
    RegistrySourceMap sources;

    for (const auto& [name, source] : config.downloader.pluginSources) {
        sources[configuration_internal::to_lower_copy(name)] = RegistrySourceEntry{.source = source};
    }

    configuration_internal::merge_registry_sources(sources, config.registry.sources);
    return sources;
}

RegistrySourceMap collect_registry_sources(const ReqPackConfig& config) {
    RegistrySourceMap sources = collect_explicit_registry_sources(config);
    configuration_internal::merge_registry_sources(sources, load_registry_sources_from_lua(registry_source_file_path(config.registry.databasePath)));

    if (!config.registry.overlayPath.empty()) {
        configuration_internal::merge_registry_sources(sources, load_registry_sources_from_lua(config.registry.overlayPath));
    }

    return sources;
}
