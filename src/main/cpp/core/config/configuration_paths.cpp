#include "configuration_internal.h"

#include <cstdlib>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr const char* ARCHIVE_PASSWORD_ENV = "REQPACK_ARCHIVE_PASSWORD";

std::optional<std::filesystem::path> passwd_home_from_user(const std::string& username) {
    if (username.empty()) {
        return std::nullopt;
    }

    if (passwd* entry = getpwnam(username.c_str())) {
        if (entry->pw_dir != nullptr && std::string(entry->pw_dir).size() > 0) {
            return std::filesystem::path(entry->pw_dir);
        }
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> passwd_home_from_uid(uid_t uid) {
    if (passwd* entry = getpwuid(uid)) {
        if (entry->pw_dir != nullptr && std::string(entry->pw_dir).size() > 0) {
            return std::filesystem::path(entry->pw_dir);
        }
    }

    return std::nullopt;
}

std::filesystem::path xdg_directory(const char* envName, const std::filesystem::path& fallback) {
    const char* value = std::getenv(envName);
    if (value != nullptr && std::string(value).size() > 0) {
        return std::filesystem::path(value) / "reqpack";
    }
    return fallback / "reqpack";
}

}  // namespace

namespace configuration_internal {

std::filesystem::path invoking_user_home_directory() {
    const char* sudoUser = std::getenv("SUDO_USER");
    if (sudoUser != nullptr && std::string(sudoUser).size() > 0) {
        if (const auto home = passwd_home_from_user(sudoUser)) {
            return home.value();
        }
    }

    const char* sudoUid = std::getenv("SUDO_UID");
    if (sudoUid != nullptr && std::string(sudoUid).size() > 0) {
        try {
            if (const auto home = passwd_home_from_uid(static_cast<uid_t>(std::stoul(sudoUid)))) {
                return home.value();
            }
        } catch (...) {
        }
    }

    const char* home = std::getenv("HOME");
    if (home != nullptr && std::string(home).size() > 0) {
        return std::filesystem::path(home);
    }

    if (const auto passwdHome = passwd_home_from_uid(getuid())) {
        return passwdHome.value();
    }

    return std::filesystem::current_path();
}

std::filesystem::path expand_user_path(const std::filesystem::path& path) {
    const std::string raw = path.string();
    if (raw.empty() || raw.front() != '~') {
        return path;
    }

    const std::filesystem::path home = invoking_user_home_directory();
    if (raw == "~") {
        return home;
    }
    if (raw.rfind("~/", 0) == 0) {
        return home / raw.substr(2);
    }

    return path;
}

std::string expand_env_reference(const std::string& value) {
    if (value.size() < 2 || value.front() != '$') {
        return value;
    }

    std::string name;
    if (value[1] == '{') {
        const std::size_t closing = value.find('}', 2);
        if (closing == std::string::npos || closing + 1 != value.size()) {
            return value;
        }
        name = value.substr(2, closing - 2);
    } else {
        name = value.substr(1);
        if (name.empty()) {
            return value;
        }
        if (!std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_';
            })) {
            return value;
        }
    }

    if (name.empty()) {
        return value;
    }

    const char* resolved = std::getenv(name.c_str());
    return resolved != nullptr ? std::string(resolved) : std::string{};
}

}  // namespace configuration_internal

ReqPackConfig::ReqPackConfig()
    : security(SecurityConfig{
          .cachePath = default_reqpack_security_cache_path().string(),
          .indexPath = default_reqpack_security_index_path().string(),
          .osvDatabasePath = default_reqpack_osv_database_path().string(),
      }),
      execution(ExecutionConfig{
          .transactionDatabasePath = default_reqpack_transaction_path().string(),
      }),
      registry(RegistryConfig{
          .databasePath = default_reqpack_registry_path().string(),
          .pluginDirectory = default_reqpack_plugin_directory().string(),
      }),
      rqp(RqpConfig{
          .statePath = default_reqpack_rqp_state_path().string(),
      }),
      selfUpdate(SelfUpdateConfig{
          .binaryDirectory = default_reqpack_self_update_binary_directory().string(),
          .linkPath = default_reqpack_self_update_link_path().string(),
      }),
      history(HistoryConfig{
           .historyPath = default_reqpack_history_path().string(),
       }) {
    version = reqpack_build_release_id();
    downloader.userAgent = reqpack_user_agent();
}

ReqPackConfig default_reqpack_config() {
    return ReqPackConfig{};
}

std::string resolve_archive_password(const ReqPackConfig& config) {
    if (!config.archives.password.empty()) {
        return config.archives.password;
    }

    const char* envPassword = std::getenv(ARCHIVE_PASSWORD_ENV);
    if (envPassword != nullptr && envPassword[0] != '\0') {
        return envPassword;
    }

    return {};
}

std::filesystem::path reqpack_config_directory() {
    return xdg_directory("XDG_CONFIG_HOME", configuration_internal::invoking_user_home_directory() / ".config");
}

std::filesystem::path reqpack_data_directory() {
    return xdg_directory("XDG_DATA_HOME", configuration_internal::invoking_user_home_directory() / ".local" / "share");
}

std::filesystem::path reqpack_cache_directory() {
    return xdg_directory("XDG_CACHE_HOME", configuration_internal::invoking_user_home_directory() / ".cache");
}

std::filesystem::path default_reqpack_config_path() {
    return reqpack_config_directory() / "config.lua";
}

std::filesystem::path default_reqpack_registry_path() {
    return reqpack_data_directory() / "registry";
}

std::filesystem::path default_reqpack_plugin_directory() {
    return reqpack_data_directory() / "plugins";
}

std::filesystem::path default_reqpack_history_path() {
    return reqpack_data_directory() / "history";
}

std::filesystem::path default_reqpack_transaction_path() {
    return reqpack_cache_directory() / "transactions";
}

std::filesystem::path default_reqpack_rqp_state_path() {
    return reqpack_data_directory() / "rqp" / "state";
}

std::filesystem::path default_reqpack_self_update_binary_directory() {
    return reqpack_data_directory() / "self" / "bin";
}

std::filesystem::path default_reqpack_self_update_link_path() {
    return configuration_internal::invoking_user_home_directory() / ".local" / "bin" / "rqp";
}

std::filesystem::path default_reqpack_security_cache_path() {
    return reqpack_cache_directory() / "security" / "cache";
}

std::filesystem::path default_reqpack_security_index_path() {
    return reqpack_data_directory() / "security" / "index";
}

std::filesystem::path default_reqpack_osv_database_path() {
    return reqpack_data_directory() / "security" / "osv";
}

std::filesystem::path default_reqpack_repo_cache_path() {
    return reqpack_data_directory() / "repos";
}
