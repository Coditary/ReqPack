#pragma once

#include "core/history/history_manager.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace history_manager_internal {

std::string utc_timestamp_now();

bool starts_with(const std::string& value, std::string_view prefix);

std::string escape_field(const std::string& value);
std::string unescape_field(const std::string& value);
std::string extract_json_string(const std::string& json, const std::string& key);

std::vector<InstalledEntry> read_legacy_installed_state(const std::filesystem::path& path);

std::string serialize_installed_entry(const InstalledEntry& entry);
std::vector<std::string> normalize_owners(std::vector<std::string> owners);
std::string install_method_from_owners(const std::vector<std::string>& owners);
std::optional<InstalledEntry> deserialize_installed_entry(const std::string& payload);

std::string installed_state_key(const std::string& system, const std::string& name, const std::string& version);
std::string installed_state_system_prefix(const std::string& system);
std::string installed_state_name_prefix(const std::string& system, const std::string& name);
std::string installed_state_identity(const InstalledEntry& entry);

} // namespace history_manager_internal
