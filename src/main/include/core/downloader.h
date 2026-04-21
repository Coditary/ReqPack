#pragma once

#include "core/configuration.h"
#include "core/registry_database.h"

#include <optional>
#include <filesystem>
#include <string>

class Downloader {
    ReqPackConfig config;
    RegistryDatabase* database;

    static std::size_t write_to_file(void* contents, std::size_t size, std::size_t nmemb, void* userp);

public:
    Downloader(RegistryDatabase* database, const ReqPackConfig& config = DEFAULT_REQPACK_CONFIG);

    bool downloadPlugin(const std::string& system) const;

private:
    bool download_to_path(const std::string& source, const std::filesystem::path& targetPath) const;
    bool ensure_registry_source_file() const;
    std::string resolve_plugin_name(const std::string& system) const;
    std::optional<RegistryRecord> plugin_record_for(const std::string& system) const;
    std::filesystem::path plugin_target_path(const std::string& system) const;
};
