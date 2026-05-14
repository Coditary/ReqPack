#include "downloader_transfer_internal.h"

#include <algorithm>
#include <chrono>

namespace downloader_transfer_internal {

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
                          CURLcode curlCode,
                          long httpStatus) {
    if (failureDetails == nullptr) {
        return;
    }

    failureDetails->source = source;
    failureDetails->remote = remote;
    failureDetails->curlCode = curlCode;
    failureDetails->httpStatus = httpStatus;
    failureDetails->message = message;
}

}  // namespace downloader_transfer_internal
