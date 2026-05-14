#pragma once

#include "core/registry/registry_database_core.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

inline constexpr unsigned int LMDB_MAX_DATABASES = 4;
inline constexpr std::size_t LMDB_MAP_SIZE = 32 * 1024 * 1024;
inline constexpr const char* REGISTRY_META_DATABASE_NAME = "meta";
inline constexpr const char* REGISTRY_META_KEY_REPO_URL = "repoUrl";
inline constexpr const char* REGISTRY_META_KEY_BRANCH = "branch";
inline constexpr const char* REGISTRY_META_KEY_PLUGINS_PATH = "remotePluginsPath";
inline constexpr const char* REGISTRY_META_KEY_SCHEMA_VERSION = "schemaVersion";
inline constexpr const char* REGISTRY_META_KEY_LAST_COMMIT = "lastCommit";

inline bool is_json_registry_remote(const ReqPackConfig& config) {
    return registry_database_is_git_source(config.registry.remoteUrl) && !config.registry.remotePluginsPath.empty();
}

struct RegistryDiffEntry {
    char status{'?'};
    std::string path;
};

std::optional<std::string> git_repository_head_commit(const std::filesystem::path& repositoryPath);
bool git_commit_exists(const std::filesystem::path& repositoryPath, const std::string& commit);
std::optional<std::vector<RegistryDiffEntry>> git_registry_diff(
    const std::filesystem::path& repositoryPath,
    const std::string& oldCommit,
    const std::string& newCommit,
    const std::string& pluginsPath
);
std::optional<std::string> latest_git_tag_for_source(const std::string& source);
bool sync_git_repository(
    const ReqPackConfig& config,
    const std::string& source,
    const std::string& pluginName,
    std::string* errorDetails = nullptr
);

std::optional<std::pair<std::string, std::string>> fetch_plugin_payload(
    const ReqPackConfig& config,
    const std::string& source,
    const std::string& pluginName
);
std::optional<std::filesystem::path> resolve_bundle_path(
    const ReqPackConfig& config,
    const std::string& source,
    const std::string& pluginName
);
std::optional<RegistryRecord> refreshed_record_payload(
    const ReqPackConfig& config,
    RegistryRecord record,
    bool preferLatestTag
);

std::optional<RegistryRecord> get_record_from_transaction(MDB_txn* transaction, MDB_dbi database, const std::string& name);
bool put_record_into_transaction(MDB_txn* transaction, MDB_dbi database, const RegistryRecord& record);
bool delete_record_from_transaction(MDB_txn* transaction, MDB_dbi database, const std::string& name);
std::optional<std::string> get_string_from_transaction(MDB_txn* transaction, MDB_dbi database, const std::string& key);
bool put_string_into_transaction(MDB_txn* transaction, MDB_dbi database, const std::string& key, const std::string& value);
bool put_meta_values_into_transaction(MDB_txn* transaction, MDB_dbi database, const std::map<std::string, std::string>& values);
