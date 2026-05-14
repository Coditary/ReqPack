#pragma once

#include "core/download/downloader.h"

#include <chrono>

namespace downloader_transfer_internal {

struct CurlDownloadProgressState {
    DownloadProgressCallback callback{nullptr};
    void* userData{nullptr};
    int lastPercent{-1};
    std::uint64_t lastBytes{0};
    std::chrono::steady_clock::time_point lastTime{};
};

int forward_download_progress(void* userp, curl_off_t downloadTotal, curl_off_t downloadNow, curl_off_t uploadTotal, curl_off_t uploadNow);

void reset_download_failure(DownloadFailureDetails* failureDetails, const std::string& source, bool remote);

void set_download_failure(DownloadFailureDetails* failureDetails,
                          const std::string& source,
                          bool remote,
                          const std::string& message,
                          CURLcode curlCode = CURLE_OK,
                          long httpStatus = 0);

}  // namespace downloader_transfer_internal
