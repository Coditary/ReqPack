#pragma once

#include "core/configuration.h"
#include "core/registry_database.h"

#include <filesystem>
#include <optional>
#include <string>

std::string registry_database_to_lower_copy(const std::string& value);
std::string registry_database_strip_query_fragment(const std::string& value);
bool registry_database_has_non_whitespace(const std::string& value);
bool registry_database_looks_like_html_document(const std::string& value);
bool registry_database_is_valid_plugin_script(const std::string& script);
bool registry_database_is_git_source(const std::string& source);
std::string registry_database_git_source_url(const std::string& source);
std::filesystem::path registry_database_git_repository_cache_path(
    const ReqPackConfig& config,
    const std::string& source,
    const std::string& pluginName
);
std::string registry_database_escape_field(const std::string& value);
std::string registry_database_unescape_field(const std::string& value);
std::string registry_database_serialize_record(const RegistryRecord& record);
std::optional<RegistryRecord> registry_database_deserialize_record(const std::string& name, const std::string& payload);
