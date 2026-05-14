#include "core/download/downloader.h"

Downloader::Downloader(RegistryDatabase* database, const ReqPackConfig& config) : config(config), database(database) {}

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
