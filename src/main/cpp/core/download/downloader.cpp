#include "core/download/downloader.h"
#include "core/download/downloader_core.h"
#include "core/plugins/plugin_bundle.h"
#include "core/registry/registry_database_core.h"

#include <curl/curl.h>

#include <cerrno>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

Downloader::Downloader(RegistryDatabase* database, const ReqPackConfig& config) : config(config), database(database) {}

namespace {

struct CurlDownloadProgressState {
    DownloadProgressCallback callback{nullptr};
    void* userData{nullptr};
    int lastPercent{-1};
    std::uint64_t lastBytes{0};
    std::chrono::steady_clock::time_point lastTime{};
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
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

bool write_script_bundle(
    const std::filesystem::path& targetDirectory,
    const std::string& pluginName,
    const std::string& description,
    const std::string& script
) {
    const std::string summary = description.empty() ? pluginName : description;
    remove_directory_contents(targetDirectory);
    return write_file(targetDirectory / "metadata.json",
               "{\n"
               "  \"formatVersion\": 1,\n"
               "  \"name\": \"" + json_escape(pluginName) + "\",\n"
               "  \"version\": \"0.0.0\",\n"
               "  \"summary\": \"" + json_escape(summary) + "\",\n"
               "  \"description\": \"" + json_escape(summary) + "\",\n"
               "  \"license\": \"unknown\"\n"
               "}\n") &&
           write_file(targetDirectory / "reqpack.lua", "return {\n  apiVersion = 1,\n  depends = {}\n}\n") &&
           write_file(targetDirectory / "run.lua", script) &&
           write_file(targetDirectory / "scripts" / "install.lua", "return true\n") &&
           write_file(targetDirectory / "scripts" / "remove.lua", "return true\n");
}

int forward_download_progress(void* userp, curl_off_t downloadTotal, curl_off_t downloadNow, curl_off_t, curl_off_t) {
    auto* state = static_cast<CurlDownloadProgressState*>(userp);
    if (state == nullptr || state->callback == nullptr) {
        return 0;
    }

    DownloadProgressSnapshot snapshot;
    if (downloadTotal > 0) {
        snapshot.totalBytes = static_cast<std::uint64_t>(downloadTotal);
        snapshot.currentBytes = static_cast<std::uint64_t>(std::max<curl_off_t>(0, downloadNow));
        snapshot.percent = std::clamp(static_cast<int>((downloadNow * 100) / downloadTotal), 0, 100);
    } else if (downloadNow > 0) {
        snapshot.currentBytes = static_cast<std::uint64_t>(downloadNow);
    }

    const auto now = std::chrono::steady_clock::now();
    if (state->lastTime != std::chrono::steady_clock::time_point{} && downloadNow >= 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state->lastTime);
        const std::uint64_t currentBytes = static_cast<std::uint64_t>(downloadNow);
        if (elapsed.count() > 0 && currentBytes >= state->lastBytes) {
            const std::uint64_t deltaBytes = currentBytes - state->lastBytes;
            const long double bytesPerSecond = (static_cast<long double>(deltaBytes) * 1000.0L) /
                                              static_cast<long double>(elapsed.count());
            if (bytesPerSecond >= 0.0L) {
                snapshot.bytesPerSecond = static_cast<std::uint64_t>(bytesPerSecond);
            }
        }
    }

    bool shouldEmit = false;
    if (snapshot.percent.has_value()) {
        shouldEmit = snapshot.percent.value() >= 100 || state->lastPercent < 0 || snapshot.percent.value() >= state->lastPercent + 1;
    } else if (snapshot.currentBytes.has_value()) {
        shouldEmit = state->lastTime == std::chrono::steady_clock::time_point{} ||
                     now - state->lastTime >= std::chrono::milliseconds(250);
    }

    if (!shouldEmit) {
        return 0;
    }

    state->lastTime = now;
    state->lastBytes = static_cast<std::uint64_t>(std::max<curl_off_t>(0, downloadNow));
    if (snapshot.percent.has_value()) {
        state->lastPercent = snapshot.percent.value();
    }

    return state->callback(snapshot, state->userData);
}

void reset_download_failure(DownloadFailureDetails* failureDetails, const std::string& source, bool remote) {
    if (failureDetails == nullptr) {
        return;
    }

    failureDetails->source = source;
    failureDetails->remote = remote;
    failureDetails->curlCode = CURLE_OK;
    failureDetails->httpStatus = 0;
    failureDetails->message.clear();
}

void set_download_failure(DownloadFailureDetails* failureDetails,
                          const std::string& source,
                          bool remote,
                          const std::string& message,
                          CURLcode curlCode = CURLE_OK,
                          long httpStatus = 0) {
    if (failureDetails == nullptr) {
        return;
    }

    failureDetails->source = source;
    failureDetails->remote = remote;
    failureDetails->curlCode = curlCode;
    failureDetails->httpStatus = httpStatus;
    failureDetails->message = message;
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
            if (const std::optional<PluginBundleLayout> layout = plugin_bundle_find_root(record->bundlePath, resolvedSystem); layout.has_value()) {
                remove_directory_contents(targetPath.parent_path());
                copy_directory_contents(layout->rootDir, targetPath.parent_path());
                return true;
            }
        }

        return write_script_bundle(targetPath.parent_path(), resolvedSystem, record->description, record->script);
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

    if (!write_script_bundle(downloadedPath.parent_path(), resolvedSystem, record->description, script)) {
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

bool Downloader::download(const std::string& source, const std::string& destinationPath) const {
	return this->download_to_path(source, destinationPath, nullptr, nullptr, nullptr);
}

bool Downloader::download(const std::string& source,
                          const std::string& destinationPath,
                          DownloadFailureDetails* failureDetails) const {
	return this->download_to_path(source, destinationPath, nullptr, nullptr, failureDetails);
}

bool Downloader::download(const std::string& source,
                         const std::string& destinationPath,
                         DownloadProgressCallback progressCallback,
                         void* progressUserData) const {
	return this->download_to_path(source, destinationPath, progressCallback, progressUserData, nullptr);
}

bool Downloader::download(const std::string& source,
                         const std::string& destinationPath,
                         DownloadProgressCallback progressCallback,
                         void* progressUserData,
                         DownloadFailureDetails* failureDetails) const {
	return this->download_to_path(source, destinationPath, progressCallback, progressUserData, failureDetails);
}

bool Downloader::download_to_path(const std::string& source, const std::filesystem::path& targetPath) const {
	return this->download_to_path(source, targetPath, nullptr, nullptr, nullptr);
}

bool Downloader::download_to_path(const std::string& source,
                                  const std::filesystem::path& targetPath,
                                  DownloadFailureDetails* failureDetails) const {
	return this->download_to_path(source, targetPath, nullptr, nullptr, failureDetails);
}

bool Downloader::download_to_path(const std::string& source,
                                  const std::filesystem::path& targetPath,
                                  DownloadProgressCallback progressCallback,
                                  void* progressUserData,
                                  DownloadFailureDetails* failureDetails) const {
    const bool remoteSource = downloader_is_remote_source(source);
    reset_download_failure(failureDetails, source, remoteSource);

    std::error_code directoryError;
    if (!targetPath.parent_path().empty()) {
        std::filesystem::create_directories(targetPath.parent_path(), directoryError);
    }
    if (directoryError) {
        set_download_failure(failureDetails,
                             source,
                             remoteSource,
                             "create_directories failed for '" + targetPath.parent_path().string() + "': " + directoryError.message());
        return false;
    }

    if (!remoteSource) {
        std::error_code error;
        std::filesystem::copy_file(source, targetPath, std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            set_download_failure(failureDetails,
                                 source,
                                 false,
                                 "copy_file failed for '" + source + "': " + error.message());
        }
        return !error;
    }

    const std::filesystem::path tempPath = downloader_temp_path_for_target(targetPath);

    FILE* file = std::fopen(tempPath.string().c_str(), "wb");
    if (file == nullptr) {
        set_download_failure(failureDetails,
                             source,
                             true,
                             "fopen failed for '" + tempPath.string() + "': " + std::strerror(errno));
        return false;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        std::fclose(file);
        set_download_failure(failureDetails, source, true, "curl_easy_init failed");
        return false;
    }

    downloader_configure_curl_handle(curl, this->config, source);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &Downloader::write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    CurlDownloadProgressState progressState{
        .callback = progressCallback,
        .userData = progressUserData,
    };
    if (progressCallback != nullptr) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &forward_download_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressState);
    }

    long statusCode = 0;
    const CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    curl_easy_cleanup(curl);
    std::fclose(file);

    if (result != CURLE_OK || statusCode >= 400) {
        std::filesystem::remove(tempPath);
        set_download_failure(failureDetails,
                             source,
                             true,
                             {},
                             result,
                             statusCode);
        return false;
    }

    std::error_code renameError;
    std::filesystem::rename(tempPath, targetPath, renameError);
    if (renameError) {
        std::filesystem::remove(tempPath);
        set_download_failure(failureDetails,
                             source,
                             true,
                             "rename failed for '" + tempPath.string() + "': " + renameError.message());
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
