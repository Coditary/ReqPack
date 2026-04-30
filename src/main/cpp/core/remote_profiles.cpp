#include "core/remote_profiles.h"

#include "core/configuration.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace {

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

RemoteProfileProtocol remote_profile_protocol_from_string(const std::string& value) {
    const std::string normalized = to_lower_copy(value);
    if (normalized == "text") {
        return RemoteProfileProtocol::TEXT;
    }
    if (normalized == "json") {
        return RemoteProfileProtocol::JSON;
    }
    if (normalized == "http") {
        return RemoteProfileProtocol::HTTP;
    }
    if (normalized == "https") {
        return RemoteProfileProtocol::HTTPS;
    }
    return RemoteProfileProtocol::AUTO;
}

std::optional<int> parse_remote_port(const std::string& value) {
    try {
        const int port = std::stoi(value);
        if (port > 0 && port <= 65535) {
            return port;
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<RemoteProfileProtocol> remote_profile_protocol_from_scheme(const std::string& value) {
    const std::string normalized = to_lower_copy(value);
    if (normalized == "tcp" || normalized == "text") {
        return RemoteProfileProtocol::TEXT;
    }
    if (normalized == "json") {
        return RemoteProfileProtocol::JSON;
    }
    if (normalized == "http") {
        return RemoteProfileProtocol::HTTP;
    }
    if (normalized == "https") {
        return RemoteProfileProtocol::HTTPS;
    }
    return std::nullopt;
}

void apply_remote_endpoint(RemoteProfile& profile, const std::string& value) {
    if (value.empty()) {
        return;
    }

    std::string endpoint = value;
    if (const std::size_t schemeSeparator = endpoint.find("://"); schemeSeparator != std::string::npos) {
        if (const std::optional<RemoteProfileProtocol> protocol =
                remote_profile_protocol_from_scheme(endpoint.substr(0, schemeSeparator));
            protocol.has_value()) {
            profile.protocol = protocol.value();
        }
        endpoint = endpoint.substr(schemeSeparator + 3);
    }

    if (const std::size_t pathSeparator = endpoint.find('/'); pathSeparator != std::string::npos) {
        endpoint = endpoint.substr(0, pathSeparator);
    }

    if (endpoint.empty()) {
        return;
    }

    if (endpoint.front() == '[') {
        const std::size_t bracketEnd = endpoint.find(']');
        if (bracketEnd != std::string::npos) {
            profile.host = endpoint.substr(1, bracketEnd - 1);
            if (bracketEnd + 2 < endpoint.size() && endpoint[bracketEnd + 1] == ':') {
                if (const std::optional<int> port = parse_remote_port(endpoint.substr(bracketEnd + 2)); port.has_value()) {
                    profile.port = port.value();
                }
            }
            return;
        }
    }

    const std::size_t firstColon = endpoint.find(':');
    if (firstColon != std::string::npos && endpoint.find(':', firstColon + 1) == std::string::npos) {
        profile.host = endpoint.substr(0, firstColon);
        if (const std::optional<int> port = parse_remote_port(endpoint.substr(firstColon + 1)); port.has_value()) {
            profile.port = port.value();
        }
        return;
    }

    profile.host = endpoint;
}

std::optional<RemoteProfile> load_remote_profile_from_table(const std::string& name, const sol::table& table) {
    RemoteProfile profile;
    profile.name = name;

    if (const sol::optional<std::string> host = table["host"]; host.has_value() && !host->empty()) {
        apply_remote_endpoint(profile, host.value());
    } else if (const sol::optional<std::string> url = table["url"]; url.has_value() && !url->empty()) {
        apply_remote_endpoint(profile, url.value());
    }

    if (const sol::optional<int> port = table["port"]; port.has_value()) {
        profile.port = port.value();
    }

    if (const sol::optional<std::string> protocol = table["protocol"]; protocol.has_value()) {
        profile.protocol = remote_profile_protocol_from_string(protocol.value());
    }

    if (const sol::optional<std::string> token = table["token"]; token.has_value() && !token->empty()) {
        profile.token = token.value();
    }
    if (const sol::optional<std::string> username = table["username"]; username.has_value() && !username->empty()) {
        profile.username = username.value();
    }
    if (const sol::optional<std::string> password = table["password"]; password.has_value() && !password->empty()) {
        profile.password = password.value();
    }

    if (profile.host.empty() || profile.port <= 0 || profile.port > 65535) {
        return std::nullopt;
    }
    return profile;
}

std::optional<RemoteUser> load_remote_user_from_table(const std::string& id, const sol::table& table) {
    RemoteUser user;
    user.id = id;

    if (const sol::optional<std::string> token = table["token"]; token.has_value() && !token->empty()) {
        user.token = token.value();
    }
    if (const sol::optional<std::string> username = table["username"]; username.has_value() && !username->empty()) {
        user.username = username.value();
    }
    if (const sol::optional<std::string> password = table["password"]; password.has_value() && !password->empty()) {
        user.password = password.value();
    }
    if (const sol::optional<bool> isAdmin = table["isAdmin"]; isAdmin.has_value()) {
        user.isAdmin = isAdmin.value();
    }

    const bool hasToken = user.token.has_value();
    const bool hasBasic = user.password.has_value();
    if (!hasToken && !hasBasic) {
        return std::nullopt;
    }
    if (hasBasic) {
        if (!user.username.has_value() || user.username->empty()) {
            user.username = id;
        }
        if (!user.username.has_value() || user.username->empty()) {
            return std::nullopt;
        }
    }

    return user;
}

std::optional<sol::table> load_remote_root_table(sol::state& lua, const std::filesystem::path& profilePath) {
    const std::filesystem::path resolvedPath = profilePath.empty() ? default_remote_profiles_path() : profilePath;
    if (!std::filesystem::exists(resolvedPath)) {
        return std::nullopt;
    }

    lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table, sol::lib::string, sol::lib::math);

    sol::load_result loadResult = lua.load_file(resolvedPath.string());
    if (!loadResult.valid()) {
        const sol::error error = loadResult;
        throw std::runtime_error("failed to parse remote profiles: " + std::string(error.what()));
    }

    const sol::protected_function_result execResult = loadResult();
    if (!execResult.valid()) {
        const sol::error error = execResult;
        throw std::runtime_error("failed to execute remote profiles: " + std::string(error.what()));
    }

    if (execResult.get_type() == sol::type::table) {
        return execResult.get<sol::table>();
    }

    const sol::object profilesObject = lua["profiles"];
    if (profilesObject.get_type() == sol::type::table) {
        return profilesObject.as<sol::table>();
    }

    const sol::object usersObject = lua["users"];
    if (usersObject.get_type() == sol::type::table) {
        sol::state_view view(lua.lua_state());
        sol::table root = view.create_table();
        root["users"] = usersObject;
        return root;
    }

    return std::nullopt;
}

std::optional<sol::table> load_named_table(sol::state& lua, const std::filesystem::path& profilePath, const char* key) {
    const std::optional<sol::table> root = load_remote_root_table(lua, profilePath);
    if (!root.has_value()) {
        return std::nullopt;
    }

    const sol::object object = root.value()[key];
    if (object.get_type() == sol::type::table) {
        return object.as<sol::table>();
    }

    if (std::string{key} == "profiles") {
        const sol::object usersObject = root.value()["users"];
        if (usersObject.get_type() != sol::type::table) {
            return root;
        }
    }

    if (std::string{key} == "users") {
        const sol::object profilesObject = root.value()["profiles"];
        if (profilesObject.get_type() != sol::type::table) {
            return root;
        }
    }

    return std::nullopt;
}

}  // namespace

std::filesystem::path default_remote_profiles_path() {
    return reqpack_home_directory() / REMOTE_PROFILES_FILENAME;
}

std::vector<RemoteProfile> load_remote_profiles(const std::filesystem::path& profilePath) {
    sol::state lua;
    const std::optional<sol::table> profilesTable = load_named_table(lua, profilePath, "profiles");
    if (!profilesTable.has_value()) {
        return {};
    }

    std::vector<RemoteProfile> profiles;
    for (const auto& [key, value] : profilesTable.value()) {
        if (key.get_type() != sol::type::string || value.get_type() != sol::type::table) {
            continue;
        }
        const std::string name = key.as<std::string>();
        const std::optional<RemoteProfile> profile = load_remote_profile_from_table(name, value.as<sol::table>());
        if (profile.has_value()) {
            profiles.push_back(profile.value());
        }
    }
    return profiles;
}

std::vector<RemoteUser> load_remote_users(const std::filesystem::path& profilePath) {
    sol::state lua;
    const std::optional<sol::table> usersTable = load_named_table(lua, profilePath, "users");
    if (!usersTable.has_value()) {
        return {};
    }

    std::vector<RemoteUser> users;
    for (const auto& [key, value] : usersTable.value()) {
        if (key.get_type() != sol::type::string || value.get_type() != sol::type::table) {
            continue;
        }
        const std::string id = key.as<std::string>();
        const std::optional<RemoteUser> user = load_remote_user_from_table(id, value.as<sol::table>());
        if (user.has_value()) {
            users.push_back(user.value());
        }
    }
    return users;
}

std::optional<RemoteProfile> find_remote_profile(
    const std::filesystem::path& profilePath,
    const std::string& profileName
) {
    const std::string normalizedName = to_lower_copy(profileName);
    for (const RemoteProfile& profile : load_remote_profiles(profilePath)) {
        if (to_lower_copy(profile.name) == normalizedName) {
            return profile;
        }
    }
    return std::nullopt;
}
