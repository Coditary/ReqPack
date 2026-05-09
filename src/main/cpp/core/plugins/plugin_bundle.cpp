#include "core/plugins/plugin_bundle.h"

#include <sol/sol.hpp>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace {

using boost::property_tree::ptree;

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trim_copy(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::optional<ptree> parse_json_tree(const std::string& json) {
    if (json.empty()) {
        return std::nullopt;
    }

    std::istringstream input(json);
    ptree tree;
    try {
        boost::property_tree::read_json(input, tree);
        return tree;
    } catch (...) {
        return std::nullopt;
    }
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string required_string(const ptree& tree, const char* key) {
    const auto value = tree.get_optional<std::string>(key);
    if (!value.has_value()) {
        return {};
    }
    return trim_copy(value.value());
}

std::vector<std::string> load_string_array(const boost::optional<const ptree&>& values, bool normalizeLower = false) {
    std::vector<std::string> result;
    if (!values.has_value()) {
        return result;
    }

    for (const auto& [_, child] : values.value()) {
        std::string raw = trim_copy(child.get_value<std::string>());
        if (raw.empty()) {
            continue;
        }
        if (normalizeLower) {
            raw = to_lower_copy(raw);
        }
        result.push_back(std::move(raw));
    }

    return result;
}

std::optional<PluginBundleMetadata> parse_metadata_json(const std::filesystem::path& path) {
    const std::optional<ptree> parsed = parse_json_tree(read_text_file(path));
    if (!parsed.has_value()) {
        return std::nullopt;
    }

    const ptree& tree = parsed.value();
    PluginBundleMetadata metadata;
    metadata.formatVersion = tree.get<int>("formatVersion", 0);
    metadata.name = to_lower_copy(required_string(tree, "name"));
    metadata.version = required_string(tree, "version");
    metadata.summary = required_string(tree, "summary");
    metadata.description = required_string(tree, "description");
    metadata.license = required_string(tree, "license");
    metadata.sourceUrl = trim_copy(tree.get<std::string>("sourceUrl", {}));
    metadata.homepage = trim_copy(tree.get<std::string>("homepage", {}));
    metadata.tags = load_string_array(tree.get_child_optional("tags"));

    if (metadata.formatVersion != 1 || metadata.name.empty() || metadata.version.empty() ||
        metadata.summary.empty() || metadata.description.empty() || metadata.license.empty()) {
        return std::nullopt;
    }

    return metadata;
}

std::optional<PluginBundleManifest> parse_reqpack_manifest(const std::filesystem::path& path) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table, sol::lib::string, sol::lib::math);

    sol::load_result loadResult = lua.load_file(path.string());
    if (!loadResult.valid()) {
        return std::nullopt;
    }

    const sol::protected_function_result execResult = loadResult();
    if (!execResult.valid()) {
        return std::nullopt;
    }

    sol::table manifestTable;
    if (execResult.return_count() > 0 && execResult.get_type() == sol::type::table) {
        manifestTable = execResult;
    } else {
        const sol::object manifestObject = lua["package"];
        if (manifestObject.get_type() == sol::type::table) {
            manifestTable = manifestObject.as<sol::table>();
        }
    }

    if (!manifestTable.valid()) {
        return std::nullopt;
    }

    const sol::optional<int> apiVersion = manifestTable["apiVersion"];
    if (!apiVersion.has_value() || apiVersion.value() != 1) {
        return std::nullopt;
    }

    PluginBundleManifest manifest;
    manifest.apiVersion = apiVersion.value();
    const sol::object dependsObject = manifestTable["depends"];
    if (dependsObject.valid() && dependsObject.get_type() == sol::type::table) {
        for (const auto& [_, value] : dependsObject.as<sol::table>()) {
            if (value.get_type() != sol::type::string) {
                return std::nullopt;
            }
            const std::string spec = trim_copy(value.as<std::string>());
            if (spec.empty()) {
                continue;
            }
            manifest.dependencySpecs.push_back(spec);
        }
    }

    return manifest;
}

std::optional<Package> parse_dependency_spec(const std::string& spec) {
    const std::string trimmed = trim_copy(spec);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    Package dependency;
    dependency.action = ActionType::INSTALL;

    std::string packagePart = trimmed;
    const std::size_t scopeSeparator = trimmed.find(':');
    if (scopeSeparator != std::string::npos) {
        if (scopeSeparator == 0 || scopeSeparator == trimmed.size() - 1) {
            return std::nullopt;
        }
        dependency.system = to_lower_copy(trim_copy(trimmed.substr(0, scopeSeparator)));
        packagePart = trim_copy(trimmed.substr(scopeSeparator + 1));
    }

    const std::size_t versionSeparator = packagePart.rfind('@');
    if (versionSeparator == std::string::npos || versionSeparator == 0 || versionSeparator == packagePart.size() - 1) {
        dependency.name = packagePart;
    } else {
        dependency.name = trim_copy(packagePart.substr(0, versionSeparator));
        dependency.version = trim_copy(packagePart.substr(versionSeparator + 1));
    }

    if (dependency.name.empty()) {
        return std::nullopt;
    }

    return dependency;
}

}  // namespace

std::optional<PluginBundleLayout> plugin_bundle_read_directory(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        return std::nullopt;
    }

    PluginBundleLayout layout;
    layout.rootDir = std::filesystem::absolute(directory).lexically_normal();
    layout.metadataPath = layout.rootDir / "metadata.json";
    layout.reqpackLuaPath = layout.rootDir / "reqpack.lua";
    layout.runScriptPath = layout.rootDir / "run.lua";
    layout.scriptsDir = layout.rootDir / "scripts";
    layout.installScriptPath = layout.scriptsDir / "install.lua";
    layout.removeScriptPath = layout.scriptsDir / "remove.lua";

    if (!std::filesystem::is_regular_file(layout.metadataPath, error) || error ||
        !std::filesystem::is_regular_file(layout.reqpackLuaPath, error) || error ||
        !std::filesystem::is_regular_file(layout.runScriptPath, error) || error ||
        !std::filesystem::is_regular_file(layout.installScriptPath, error) || error ||
        !std::filesystem::is_regular_file(layout.removeScriptPath, error) || error) {
        return std::nullopt;
    }

    const std::optional<PluginBundleMetadata> metadata = parse_metadata_json(layout.metadataPath);
    if (!metadata.has_value()) {
        return std::nullopt;
    }
    const std::optional<PluginBundleManifest> manifest = parse_reqpack_manifest(layout.reqpackLuaPath);
    if (!manifest.has_value()) {
        return std::nullopt;
    }

    layout.metadata = metadata.value();
    layout.manifest = manifest.value();
    return layout;
}

std::optional<PluginBundleLayout> plugin_bundle_find_root(const std::filesystem::path& basePath, const std::string& expectedPluginId) {
    std::vector<std::filesystem::path> candidates;
    if (!expectedPluginId.empty()) {
        candidates.push_back(basePath / "plugins" / expectedPluginId);
        candidates.push_back(basePath / expectedPluginId);
    }
    candidates.push_back(basePath);

    const std::string expected = to_lower_copy(expectedPluginId);
    for (const std::filesystem::path& candidate : candidates) {
        const std::optional<PluginBundleLayout> layout = plugin_bundle_read_directory(candidate);
        if (!layout.has_value()) {
            continue;
        }
        if (!expected.empty() && layout->metadata.name != expected) {
            continue;
        }
        return layout;
    }

    return std::nullopt;
}

std::optional<PluginBundleLayout> plugin_bundle_find_installed(const ReqPackConfig& config, const std::string& pluginId) {
    const std::string normalized = to_lower_copy(pluginId);
    if (normalized.empty()) {
        return std::nullopt;
    }

    const std::filesystem::path configuredPath = std::filesystem::path(config.registry.pluginDirectory) / normalized;
    if (const std::optional<PluginBundleLayout> layout = plugin_bundle_read_directory(configuredPath); layout.has_value()) {
        return layout;
    }

    std::error_code error;
    const std::filesystem::path workspacePath = std::filesystem::current_path(error) / "plugins" / normalized;
    if (!error && workspacePath != configuredPath) {
        if (const std::optional<PluginBundleLayout> layout = plugin_bundle_read_directory(workspacePath); layout.has_value()) {
            return layout;
        }
    }

    return std::nullopt;
}

std::vector<Package> plugin_bundle_dependency_packages(const PluginBundleLayout& layout) {
    std::vector<Package> dependencies;
    dependencies.reserve(layout.manifest.dependencySpecs.size());
    for (const std::string& spec : layout.manifest.dependencySpecs) {
        const std::optional<Package> dependency = parse_dependency_spec(spec);
        if (!dependency.has_value()) {
            return {};
        }
        dependencies.push_back(dependency.value());
    }
    return dependencies;
}

std::filesystem::path plugin_bundle_ready_marker_path(const std::filesystem::path& directory) {
    return directory / ".requirements_ready";
}

std::filesystem::path plugin_bundle_manifest_path(const std::filesystem::path& directory) {
    return directory / ".plugin-manifest.json";
}

std::filesystem::path plugin_bundle_source_path(const std::filesystem::path& directory) {
    return directory / ".plugin-source.json";
}
