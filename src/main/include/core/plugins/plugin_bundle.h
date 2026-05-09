#pragma once

#include "core/common/types.h"
#include "core/config/configuration.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct PluginBundleMetadata {
    int formatVersion{0};
    std::string name;
    std::string version;
    std::string summary;
    std::string description;
    std::string license;
    std::string sourceUrl;
    std::string homepage;
    std::vector<std::string> tags;
};

struct PluginBundleManifest {
    int apiVersion{0};
    std::vector<std::string> dependencySpecs;
};

struct PluginBundleLayout {
    PluginBundleMetadata metadata;
    PluginBundleManifest manifest;
    std::filesystem::path rootDir;
    std::filesystem::path metadataPath;
    std::filesystem::path reqpackLuaPath;
    std::filesystem::path runScriptPath;
    std::filesystem::path scriptsDir;
    std::filesystem::path installScriptPath;
    std::filesystem::path removeScriptPath;
};

std::optional<PluginBundleLayout> plugin_bundle_read_directory(const std::filesystem::path& directory);
std::optional<PluginBundleLayout> plugin_bundle_find_root(const std::filesystem::path& basePath, const std::string& expectedPluginId = {});
std::optional<PluginBundleLayout> plugin_bundle_find_installed(const ReqPackConfig& config, const std::string& pluginId);
std::vector<Package> plugin_bundle_dependency_packages(const PluginBundleLayout& layout);
std::filesystem::path plugin_bundle_ready_marker_path(const std::filesystem::path& directory);
std::filesystem::path plugin_bundle_manifest_path(const std::filesystem::path& directory);
std::filesystem::path plugin_bundle_source_path(const std::filesystem::path& directory);
