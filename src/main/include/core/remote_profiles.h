#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

enum class RemoteProfileProtocol {
    AUTO,
    TEXT,
    JSON,
    HTTP,
    HTTPS
};

struct RemoteProfile {
    std::string name;
    std::string host{"127.0.0.1"};
    int port{4545};
    RemoteProfileProtocol protocol{RemoteProfileProtocol::AUTO};
    std::optional<std::string> token;
    std::optional<std::string> username;
    std::optional<std::string> password;
};

struct RemoteUser {
    std::string id;
    std::optional<std::string> token;
    std::optional<std::string> username;
    std::optional<std::string> password;
    bool isAdmin{false};
};

inline const std::string REMOTE_PROFILES_FILENAME = "remote.lua";

std::filesystem::path default_remote_profiles_path();
std::vector<RemoteProfile> load_remote_profiles(const std::filesystem::path& profilePath);
std::vector<RemoteUser> load_remote_users(const std::filesystem::path& profilePath);
std::optional<RemoteProfile> find_remote_profile(
    const std::filesystem::path& profilePath,
    const std::string& profileName
);
