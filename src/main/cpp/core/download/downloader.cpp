#include "core/download/downloader.h"
#include "core/download/downloader_core.h"
#include "core/registry/registry_database_core.h"

#include <curl/curl.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

Downloader::Downloader(RegistryDatabase* database, const ReqPackConfig& config) : config(config), database(database) {}

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        return false;
    }

    const std::size_t written = std::fwrite(content.data(), 1, content.size(), file);
    std::fclose(file);
    return written == content.size();
}

void remove_directory_contents(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::exists(directory)) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            return;
        }
        std::filesystem::remove_all(entry.path(), error);
        if (error) {
            return;
        }
    }
}

void copy_directory_contents(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code error;
    std::filesystem::create_directories(target, error);
    if (error) {
        return;
    }

    for (auto it = std::filesystem::recursive_directory_iterator(source, error); it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (error) {
            return;
        }

        const std::filesystem::directory_entry& entry = *it;

        const std::filesystem::path relativePath = std::filesystem::relative(entry.path(), source, error);
        if (error) {
            return;
        }

        if (!relativePath.empty() && *relativePath.begin() == ".git") {
            if (entry.is_directory()) {
                it.disable_recursion_pending();
            }
            continue;
        }

        const std::filesystem::path targetPath = target / relativePath;
        if (entry.is_directory()) {
            std::filesystem::create_directories(targetPath, error);
            if (error) {
                return;
            }
            continue;
        }

        std::filesystem::create_directories(targetPath.parent_path(), error);
        if (error) {
            return;
        }

        std::filesystem::copy_file(entry.path(), targetPath, std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            return;
        }
    }
}

}  // namespace

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

        if (record->bundleSource && !record->bundlePath.empty() && std::filesystem::exists(record->bundlePath)) {
            remove_directory_contents(targetPath.parent_path());
            copy_directory_contents(record->bundlePath, targetPath.parent_path());
            return true;
        }

        if (!write_file(targetPath, record->script)) {
            return false;
        }

        const std::filesystem::path bootstrapPath = targetPath.parent_path() / "bootstrap.lua";
        if (!record->bootstrapScript.empty()) {
            return write_file(bootstrapPath, record->bootstrapScript);
        }

        std::error_code removeError;
        std::filesystem::remove(bootstrapPath, removeError);
        return true;
    }

    if (!this->download_to_path(record->source, targetPath)) {
        return false;
    }

    const std::filesystem::path downloadedPath = this->plugin_target_path(resolvedSystem);
    const std::string script = read_file(downloadedPath);
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

    if (const std::optional<RegistryRecord> refreshedRecord = this->database->getRecord(resolvedSystem)) {
        if (!script.empty()) {
            (void)this->database->cacheScript(refreshedRecord->name, script);
        }
    }

    return true;
}

bool Downloader::download(const std::string& source, const std::string& destinationPath) const {
	return this->download_to_path(source, destinationPath);
}

bool Downloader::download_to_path(const std::string& source, const std::filesystem::path& targetPath) const {
    std::filesystem::create_directories(targetPath.parent_path());

    if (!downloader_is_remote_source(source)) {
        std::error_code error;
        std::filesystem::copy_file(source, targetPath, std::filesystem::copy_options::overwrite_existing, error);
        return !error;
    }

    const std::filesystem::path tempPath = downloader_temp_path_for_target(targetPath);

    FILE* file = std::fopen(tempPath.string().c_str(), "wb");
    if (file == nullptr) {
        return false;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        std::fclose(file);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, source.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, this->config.downloader.followRedirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, this->config.downloader.connectTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, this->config.downloader.requestTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, this->config.downloader.userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &Downloader::write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

    long statusCode = 0;
    const CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    curl_easy_cleanup(curl);
    std::fclose(file);

    if (result != CURLE_OK || statusCode >= 400) {
        std::filesystem::remove(tempPath);
        return false;
    }

    std::error_code renameError;
    std::filesystem::rename(tempPath, targetPath, renameError);
    if (renameError) {
        std::filesystem::remove(tempPath);
        return false;
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

std::size_t Downloader::write_to_file(void* contents, std::size_t size, std::size_t nmemb, void* userp) {
    return std::fwrite(contents, size, nmemb, static_cast<FILE*>(userp));
}
