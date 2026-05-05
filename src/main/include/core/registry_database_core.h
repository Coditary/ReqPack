#pragma once

#include "core/configuration.h"
#include "core/registry_database.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

std::string registry_database_to_lower_copy(const std::string& value);
std::string registry_database_strip_query_fragment(const std::string& value);
bool registry_database_has_non_whitespace(const std::string& value);
bool registry_database_looks_like_html_document(const std::string& value);
bool registry_database_is_valid_plugin_script(const std::string& script);
bool registry_database_is_valid_sha256(const std::string& value);
std::string registry_database_sha256_hex(const std::string& value);
std::string registry_database_serialize_write_scopes(const std::vector<RegistryWriteScope>& values);
std::vector<RegistryWriteScope> registry_database_deserialize_write_scopes(const std::string& value);
std::string registry_database_serialize_network_scopes(const std::vector<RegistryNetworkScope>& values);
std::vector<RegistryNetworkScope> registry_database_deserialize_network_scopes(const std::string& value);
bool registry_database_is_git_source(const std::string& source);
std::string registry_database_git_source_url(const std::string& source);
std::string registry_database_git_source_ref(const std::string& source);
std::string registry_database_git_source_with_ref(const std::string& source, const std::string& ref);
std::filesystem::path registry_database_git_repository_cache_path(
    const ReqPackConfig& config,
    const std::string& source,
    const std::string& pluginName
);
std::vector<std::string> registry_database_extract_git_tags(const std::string& output);
std::string registry_database_escape_field(const std::string& value);
std::string registry_database_unescape_field(const std::string& value);
bool registry_record_passes_thin_layer_trust(const ReqPackConfig& config, const RegistryRecord& record);
bool registry_record_matches_expected_hashes(const RegistryRecord& record);
std::string registry_database_serialize_record(const RegistryRecord& record);
std::optional<RegistryRecord> registry_database_deserialize_record(const std::string& name, const std::string& payload);
