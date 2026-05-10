#pragma once

#include "core/config/configuration.h"
#include "core/registry/registry_database.h"

#include <curl/curl.h>

#include <optional>
#include <filesystem>
#include <string>

struct DownloadProgressSnapshot {
    std::optional<int> percent{};
    std::optional<std::uint64_t> currentBytes{};
    std::optional<std::uint64_t> totalBytes{};
    std::optional<std::uint64_t> bytesPerSecond{};
};

using DownloadProgressCallback = int(*)(const DownloadProgressSnapshot& snapshot, void* userData);

class Downloader {
    ReqPackConfig config;
    RegistryDatabase* database;

    static std::size_t write_to_file(void* contents, std::size_t size, std::size_t nmemb, void* userp);

public:
    Downloader(RegistryDatabase* database, const ReqPackConfig& config = default_reqpack_config());

    bool downloadPlugin(const std::string& system) const;
    bool download(const std::string& source, const std::string& destinationPath) const;
    bool download(const std::string& source,
                  const std::string& destinationPath,
                  DownloadProgressCallback progressCallback,
                  void* progressUserData) const;

private:
    bool download_to_path(const std::string& source, const std::filesystem::path& targetPath) const;
    bool download_to_path(const std::string& source,
                          const std::filesystem::path& targetPath,
                          DownloadProgressCallback progressCallback,
                          void* progressUserData) const;
    std::string resolve_plugin_name(const std::string& system) const;
    std::optional<RegistryRecord> plugin_record_for(const std::string& system) const;
    std::filesystem::path plugin_target_path(const std::string& system) const;
};
