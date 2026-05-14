#include "registry_database_internal.h"

#include "core/registry/registry_json_parser.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <map>
#include <set>

namespace {

std::vector<std::filesystem::path> collect_registry_json_files(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!std::filesystem::exists(root, error) || error) {
        return files;
    }

    for (auto it = std::filesystem::recursive_directory_iterator(root, error);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
        if (error) {
            return {};
        }
        if (!it->is_regular_file() || it->path().extension() != ".json") {
            continue;
        }
        files.push_back(it->path());
    }

    std::sort(files.begin(), files.end());
    return files;
}

std::string path_to_generic_string(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

bool path_has_json_extension(const std::string& path) {
    return std::filesystem::path(path).extension() == ".json";
}

}  // namespace

bool RegistryDatabase::bootstrap_registry() const {
    const RegistrySourceMap explicitSources = collect_explicit_registry_sources(this->config);
    const auto seedExplicitSources = [&]() {
        return explicitSources.empty() || this->write_records(explicitSources);
    };

    if (!is_json_registry_remote(this->config)) {
        return seedExplicitSources();
    }

    const bool mainRegistryReady = this->sync_main_registry();

    const bool explicitReady = seedExplicitSources();
    return explicitReady && (mainRegistryReady || !explicitSources.empty());
}

bool RegistryDatabase::sync_main_registry(bool* changed) const {
    if (changed != nullptr) {
        *changed = false;
    }

    if (!is_json_registry_remote(this->config)) {
        return true;
    }

    const std::string source = registry_database_git_source_with_ref(
        this->config.registry.remoteUrl,
        this->config.registry.remoteBranch
    );
    if (!sync_git_repository(this->config, source, "main-registry")) {
        return !this->load_all_records().empty();
    }

    const std::filesystem::path repositoryPath = registry_database_git_repository_cache_path(this->config, source, "main-registry");
    const std::filesystem::path pluginsRoot = repositoryPath / this->config.registry.remotePluginsPath;
    if (!std::filesystem::exists(pluginsRoot)) {
        return !this->load_all_records().empty();
    }

    const std::optional<std::string> currentCommit = git_repository_head_commit(repositoryPath);
    if (!currentCommit.has_value() || currentCommit->empty()) {
        return !this->load_all_records().empty();
    }

    const std::map<std::string, std::string> baseMetaValues = {
        {REGISTRY_META_KEY_REPO_URL, this->config.registry.remoteUrl},
        {REGISTRY_META_KEY_BRANCH, this->config.registry.remoteBranch},
        {REGISTRY_META_KEY_PLUGINS_PATH, this->config.registry.remotePluginsPath},
        {REGISTRY_META_KEY_SCHEMA_VERSION, "1"},
        {REGISTRY_META_KEY_LAST_COMMIT, *currentCommit},
    };

    const std::optional<std::string> previousCommit = this->load_meta_value(REGISTRY_META_KEY_LAST_COMMIT);
    if (!previousCommit.has_value() || previousCommit->empty() || !git_commit_exists(repositoryPath, *previousCommit)) {
        std::vector<RegistryRecord> records;
        try {
            for (const std::filesystem::path& file : collect_registry_json_files(pluginsRoot)) {
                const RegistryJsonParseResult parsed = parse_registry_json_file(file);
                records.insert(records.end(), parsed.records.begin(), parsed.records.end());
            }
        } catch (const std::exception&) {
            return !this->load_all_records().empty();
        }

        if (changed != nullptr && (!previousCommit.has_value() || *previousCommit != *currentCommit)) {
            *changed = true;
        }
        const bool synced = this->sync_records(records, false, true, baseMetaValues);
        return synced || !this->load_all_records().empty();
    }

    if (*previousCommit == *currentCommit) {
        return this->sync_records({}, false, false, baseMetaValues);
    }

    const std::optional<std::vector<RegistryDiffEntry>> diffEntries = git_registry_diff(
        repositoryPath,
        *previousCommit,
        *currentCommit,
        this->config.registry.remotePluginsPath
    );
    if (!diffEntries.has_value()) {
        std::vector<RegistryRecord> records;
        try {
            for (const std::filesystem::path& file : collect_registry_json_files(pluginsRoot)) {
                const RegistryJsonParseResult parsed = parse_registry_json_file(file);
                records.insert(records.end(), parsed.records.begin(), parsed.records.end());
            }
        } catch (const std::exception&) {
            return !this->load_all_records().empty();
        }

        if (changed != nullptr) {
            *changed = true;
        }
        const bool synced = this->sync_records(records, false, true, baseMetaValues);
        return synced || !this->load_all_records().empty();
    }

    std::set<std::string> originPathsToDelete;
    std::set<std::string> parsePaths;
    for (const RegistryDiffEntry& entry : *diffEntries) {
        if (!path_has_json_extension(entry.path)) {
            continue;
        }

        const std::filesystem::path absolutePath = repositoryPath / entry.path;
        if (entry.status == 'D' || entry.status == 'R') {
            originPathsToDelete.insert(path_to_generic_string(absolutePath));
            continue;
        }

        if (!std::filesystem::exists(absolutePath)) {
            originPathsToDelete.insert(path_to_generic_string(absolutePath));
            continue;
        }

        originPathsToDelete.insert(path_to_generic_string(absolutePath));
        parsePaths.insert(path_to_generic_string(absolutePath));
    }

    std::vector<RegistryRecord> records;
    try {
        for (const std::string& path : parsePaths) {
            const RegistryJsonParseResult parsed = parse_registry_json_file(path);
            records.insert(records.end(), parsed.records.begin(), parsed.records.end());
        }
    } catch (const std::exception&) {
        return !this->load_all_records().empty();
    }

    if (changed != nullptr) {
        *changed = true;
    }
    const bool synced = this->sync_records(
        records,
        false,
        false,
        baseMetaValues,
        std::vector<std::string>(originPathsToDelete.begin(), originPathsToDelete.end())
    );
    return synced || !this->load_all_records().empty();
}
