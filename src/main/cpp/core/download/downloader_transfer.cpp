#include "core/download/downloader.h"

#include "downloader_transfer_internal.h"

#include "core/download/downloader_core.h"

#include <curl/curl.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

std::size_t Downloader::write_to_file(void* contents, std::size_t size, std::size_t nmemb, void* userp) {
    return std::fwrite(contents, size, nmemb, static_cast<FILE*>(userp));
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
    downloader_transfer_internal::reset_download_failure(failureDetails, source, remoteSource);

    std::error_code directoryError;
    if (!targetPath.parent_path().empty()) {
        std::filesystem::create_directories(targetPath.parent_path(), directoryError);
    }
    if (directoryError) {
        downloader_transfer_internal::set_download_failure(failureDetails,
                                                           source,
                                                           remoteSource,
                                                           "create_directories failed for '" + targetPath.parent_path().string() + "': " + directoryError.message());
        return false;
    }

    if (!remoteSource) {
        std::error_code error;
        std::filesystem::copy_file(source, targetPath, std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            downloader_transfer_internal::set_download_failure(failureDetails,
                                                               source,
                                                               false,
                                                               "copy_file failed for '" + source + "': " + error.message());
        }
        return !error;
    }

    const std::filesystem::path tempPath = downloader_temp_path_for_target(targetPath);

    FILE* file = std::fopen(tempPath.string().c_str(), "wb");
    if (file == nullptr) {
        downloader_transfer_internal::set_download_failure(failureDetails,
                                                           source,
                                                           true,
                                                           "fopen failed for '" + tempPath.string() + "': " + std::strerror(errno));
        return false;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        std::fclose(file);
        downloader_transfer_internal::set_download_failure(failureDetails, source, true, "curl_easy_init failed");
        return false;
    }

    downloader_configure_curl_handle(curl, this->config, source);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &Downloader::write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    downloader_transfer_internal::CurlDownloadProgressState progressState{
        .callback = progressCallback,
        .userData = progressUserData,
    };
    if (progressCallback != nullptr) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &downloader_transfer_internal::forward_download_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressState);
    }

    long statusCode = 0;
    const CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    curl_easy_cleanup(curl);
    std::fclose(file);

    if (result != CURLE_OK || statusCode >= 400) {
        std::filesystem::remove(tempPath);
        downloader_transfer_internal::set_download_failure(failureDetails,
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
        downloader_transfer_internal::set_download_failure(failureDetails,
                                                           source,
                                                           true,
                                                           "rename failed for '" + tempPath.string() + "': " + renameError.message());
        return false;
    }

    return true;
}
