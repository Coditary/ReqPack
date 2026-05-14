#pragma once

#include "core/registry/registry_database.h"

#include <filesystem>
#include <string>

namespace downloader_plugin_internal {

std::string read_file(const std::filesystem::path& path);

bool materialize_script_record_bundle(
    const std::filesystem::path& targetDirectory,
    const std::string& pluginName,
    const RegistryRecord& record
);

bool write_script_bundle(
    const std::filesystem::path& targetDirectory,
    const std::string& pluginName,
    const std::string& description,
    const std::string& script
);

}  // namespace downloader_plugin_internal
