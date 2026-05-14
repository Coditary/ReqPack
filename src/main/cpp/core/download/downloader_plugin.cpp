#include "core/download/downloader.h"

#include "downloader_plugin_internal.h"

#include "core/download/downloader_core.h"
#include "core/registry/registry_database_core.h"

#include <optional>

bool Downloader::downloadPlugin(const std::string& system) const {
    if (!this->config.downloader.enabled) {
        return false;
    }

    if (this->database == nullptr || !this->database->ensureReady()) {
        return false;
    }

    const std::string resolvedSystem = this->resolve_plugin_name(system);

    std::optional<RegistryRecord> record = this->plugin_record_for(system);
    if (!record.has_value() && resolvedSystem != system) {
        record = this->plugin_record_for(resolvedSystem);
    }

    if (!record.has_value() || record->source.empty()) {
        return false;
    }

    if (!registry_record_passes_thin_layer_trust(this->config, record.value())) {
        return false;
    }

    if (record->script.empty() && !record->alias) {
        const std::optional<RegistryRecord> refreshed = this->database->refreshRecord(resolvedSystem);
        if (!refreshed.has_value()) {
            return false;
        }
        record = refreshed;
    }

    const std::filesystem::path targetPath = this->plugin_target_path(resolvedSystem);

    if (!record->script.empty()) {
        if (!registry_record_matches_expected_hashes(record.value())) {
            return false;
        }

        return downloader_plugin_internal::materialize_script_record_bundle(targetPath.parent_path(), resolvedSystem, record.value());
    }

    if (!this->download_to_path(record->source, targetPath)) {
        return false;
    }

    const std::filesystem::path downloadedPath = this->plugin_target_path(resolvedSystem);
    const std::string script = downloader_plugin_internal::read_file(downloadedPath);
    if (!downloader_is_valid_plugin_script(script)) {
        std::error_code removeError;
        std::filesystem::remove(downloadedPath, removeError);
        return false;
    }

    RegistryRecord downloadedRecord = record.value();
    downloadedRecord.script = script;
    downloadedRecord.bootstrapScript.clear();
    downloadedRecord.bundleSource = false;
    downloadedRecord.bundlePath.clear();
    if (!registry_record_matches_expected_hashes(downloadedRecord)) {
        std::error_code removeError;
        std::filesystem::remove(downloadedPath, removeError);
        return false;
    }

    if (!downloader_plugin_internal::write_script_bundle(downloadedPath.parent_path(), resolvedSystem, record->description, script)) {
        std::error_code removeError;
        std::filesystem::remove_all(downloadedPath.parent_path(), removeError);
        return false;
    }

    if (const std::optional<RegistryRecord> refreshedRecord = this->database->getRecord(resolvedSystem)) {
        if (!script.empty()) {
            (void)this->database->cacheScript(refreshedRecord->name, script);
        }
    }

    return true;
}

std::string Downloader::resolve_plugin_name(const std::string& system) const {
    auto normalized = downloader_to_lower_copy(system);

    if (const std::optional<RegistryRecord> record = this->database->getRecord(normalized)) {
        if (record->alias && !record->source.empty()) {
            return record->source;
        }
        return record->name;
    }

    auto alias = this->config.planner.systemAliases.find(normalized);
    if (alias != this->config.planner.systemAliases.end()) {
        return alias->second;
    }

    return normalized;
}

std::optional<RegistryRecord> Downloader::plugin_record_for(const std::string& system) const {
    auto normalized = downloader_to_lower_copy(system);

    if (this->database == nullptr) {
        return std::nullopt;
    }

    return this->database->resolveRecord(normalized);
}

std::filesystem::path Downloader::plugin_target_path(const std::string& system) const {
    return downloader_plugin_target_path(this->config, system);
}
