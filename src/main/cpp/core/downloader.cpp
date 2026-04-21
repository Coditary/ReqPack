#include "core/downloader.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
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

bool has_non_whitespace(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    });
}

bool looks_like_html_document(const std::string& value) {
    std::string prefix = value.substr(0, std::min<std::size_t>(value.size(), 512));
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    return prefix.find("<!doctype html") != std::string::npos ||
           prefix.find("<html") != std::string::npos ||
           prefix.find("<head") != std::string::npos ||
           prefix.find("<body") != std::string::npos;
}

bool is_valid_plugin_script(const std::string& script) {
    return has_non_whitespace(script) && !looks_like_html_document(script);
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

}  // namespace

bool Downloader::downloadPlugin(const std::string& system) const {
    if (!this->config.downloader.enabled) {
        return false;
    }

    if (this->database == nullptr || !this->database->ensureReady()) {
        return false;
    }

    std::optional<RegistryRecord> record = this->plugin_record_for(system);
    if (!record.has_value() && !this->ensure_registry_source_file()) {
        return false;
    }

    if (!record.has_value()) {
        record = this->plugin_record_for(system);
    }

    if (!record.has_value() || record->source.empty()) {
        return false;
    }

    const std::string resolvedSystem = this->resolve_plugin_name(system);
    const std::filesystem::path targetPath = this->plugin_target_path(resolvedSystem);

    if (!record->script.empty()) {
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
    if (!is_valid_plugin_script(script)) {
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

bool Downloader::download_to_path(const std::string& source, const std::filesystem::path& targetPath) const {
    std::filesystem::create_directories(targetPath.parent_path());

    if (source.find("://") == std::string::npos) {
        std::error_code error;
        std::filesystem::copy_file(source, targetPath, std::filesystem::copy_options::overwrite_existing, error);
        return !error;
    }

    const std::filesystem::path tempPath = targetPath.string() + ".tmp";

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

bool Downloader::ensure_registry_source_file() const {
    const std::filesystem::path registrySourcePath = registry_source_file_path(this->config.registry.databasePath);
    if (std::filesystem::exists(registrySourcePath)) {
        return true;
    }

    if (this->config.registry.remoteUrl.empty()) {
        return true;
    }

    return this->download_to_path(this->config.registry.remoteUrl, registrySourcePath);
}

std::string Downloader::resolve_plugin_name(const std::string& system) const {
    auto normalized = system;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

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
    auto normalized = system;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (this->database == nullptr) {
        return std::nullopt;
    }

    return this->database->resolveRecord(normalized);
}

std::filesystem::path Downloader::plugin_target_path(const std::string& system) const {
    return std::filesystem::path(this->config.registry.pluginDirectory) / system / (system + ".lua");
}

std::size_t Downloader::write_to_file(void* contents, std::size_t size, std::size_t nmemb, void* userp) {
    return std::fwrite(contents, size, nmemb, static_cast<FILE*>(userp));
}
