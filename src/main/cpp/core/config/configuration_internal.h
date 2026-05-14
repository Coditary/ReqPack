#pragma once

#include "core/config/configuration.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace configuration_internal {

inline std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

inline std::optional<unsigned int> unsigned_int_from_string(const std::string& value) {
    std::string trimmed = value;
    trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(), [](unsigned char c) {
        return !std::isspace(c);
    }));
    trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), [](unsigned char c) {
        return !std::isspace(c);
    }).base(), trimmed.end());
    if (trimmed.empty()) {
        return std::nullopt;
    }

    try {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(trimmed, &consumed, 10);
        if (consumed != trimmed.size() || parsed == 0 || parsed > std::numeric_limits<unsigned int>::max()) {
            return std::nullopt;
        }
        return static_cast<unsigned int>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

inline std::vector<std::string> normalize_string_list(std::vector<std::string> values) {
    for (std::string& value : values) {
        value = to_lower_copy(value);
    }
    values.erase(std::remove_if(values.begin(), values.end(), [](const std::string& value) {
        return value.empty();
    }), values.end());
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

std::filesystem::path invoking_user_home_directory();
std::filesystem::path expand_user_path(const std::filesystem::path& path);
std::string expand_env_reference(const std::string& value);

std::map<std::string, std::vector<std::string>> load_string_list_map(const sol::object& object, bool& ok);
bool load_string_array_strict(const sol::object& object, std::vector<std::string>& values);
std::vector<std::string> load_string_array(const sol::object& object);
std::vector<RegistryWriteScope> load_registry_write_scopes(const sol::object& object);
std::vector<RegistryNetworkScope> load_registry_network_scopes(const sol::object& object);
RegistrySourceMap load_registry_sources_from_table(const sol::table& table);
std::map<std::string, std::string> load_string_map(const sol::object& object);
std::map<std::string, ProxyConfig> load_proxy_config_map(const sol::object& object);
std::map<std::string, SecurityGatewayConfig> load_security_gateway_map(const sol::object& object);
std::map<std::string, SecurityBackendConfig> load_security_backend_map(const sol::object& object);
std::map<std::string, std::vector<RepositoryEntry>> load_repository_map(const sol::object& object);
void merge_registry_sources(RegistrySourceMap& target, const RegistrySourceMap& source);

template <typename Enum>
void assign_if_present(const sol::table& table, const char* key, std::optional<Enum> (*converter)(const std::string&), Enum& target) {
    const sol::optional<std::string> value = table[key];
    if (!value.has_value()) {
        return;
    }

    const std::optional<Enum> converted = converter(value.value());
    if (converted.has_value()) {
        target = converted.value();
    }
}

template <typename T>
void assign_if_present(const sol::table& table, const char* key, T& target) {
    const sol::optional<T> value = table[key];
    if (value.has_value()) {
        target = value.value();
    }
}

}  // namespace configuration_internal
