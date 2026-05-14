#pragma once

#include "main_self_update.h"

#include "core/download/downloader.h"
#include "core/host/host_info.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Logger;

namespace self_update_internal {

std::string trim_copy(const std::string& value);
std::optional<std::string> trim_line(const std::string& value);
bool run_process(const std::vector<std::string>& arguments, const std::filesystem::path& workingDirectory);
bool ensure_directory(const std::filesystem::path& path);
bool copy_directory_contents_with_mode(const std::filesystem::path& sourceRoot, const std::filesystem::path& targetRoot);
bool replace_symlink_atomically(const std::filesystem::path& target, const std::filesystem::path& linkPath);
std::string sanitize_release_identifier(std::string value);
std::optional<std::filesystem::path> create_self_update_temp_directory();
bool remove_path_quietly(const std::filesystem::path& path);
std::string shell_escape_arg(const std::string& value);
std::vector<std::string> split_non_empty_lines(const std::string& value);

std::optional<std::pair<std::string, std::string>> parse_release_owner_repo(const std::string& repoUrl);
std::optional<std::string> self_update_release_target(const HostInfoSnapshot& snapshot);
std::string self_update_release_api_url(const SelfUpdateConfig& config, const std::string& owner, const std::string& repo);
std::string self_update_download_failure_details(const std::string& url, const DownloadFailureDetails& failureDetails);
std::string self_update_missing_asset_details(const SelfUpdateConfig& config,
                                             const std::filesystem::path& metadataPath,
                                             const std::string& releaseTarget);
std::optional<std::pair<std::string, std::string>> resolve_self_update_asset(
    const std::string& owner,
    const std::string& repo,
    const std::string& releaseTarget,
    const std::filesystem::path& metadataPath
);
bool extract_release_archive(const std::filesystem::path& archivePath,
                            const std::filesystem::path& destinationPath,
                            const std::function<void(std::size_t, std::size_t)>& onEntryExtracted = {});
std::optional<std::filesystem::path> locate_extracted_binary(const std::filesystem::path& directory);
std::optional<std::filesystem::path> current_link_target_path(const std::filesystem::path& linkPath);
bool refresh_update_registry(const ReqPackConfig& config, Logger& logger, bool warnOnFailure);

}  // namespace self_update_internal
