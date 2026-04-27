#pragma once

#include "core/configuration.h"

#include <filesystem>
#include <string>

std::string downloader_to_lower_copy(const std::string& value);
bool downloader_has_non_whitespace(const std::string& value);
bool downloader_looks_like_html_document(const std::string& value);
bool downloader_is_valid_plugin_script(const std::string& script);
bool downloader_is_remote_source(const std::string& source);
std::filesystem::path downloader_temp_path_for_target(const std::filesystem::path& targetPath);
std::filesystem::path downloader_plugin_target_path(const ReqPackConfig& config, const std::string& system);
