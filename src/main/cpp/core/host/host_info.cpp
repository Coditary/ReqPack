#include "core/host/host_info.h"

#include "core/config/configuration.h"

#include "host_info_internal.h"

#include <mutex>

namespace {

std::mutex g_host_info_mutex;
std::shared_ptr<const HostInfoSnapshot> g_cached_snapshot;

}  // namespace

std::filesystem::path default_reqpack_host_info_cache_path() {
    return reqpack_cache_directory() / "host" / "info.v1.json";
}

std::shared_ptr<const HostInfoSnapshot> HostInfoService::currentSnapshot() {
    std::scoped_lock lock(g_host_info_mutex);
    if (g_cached_snapshot) {
        return g_cached_snapshot;
    }

    const std::filesystem::path cachePath = default_reqpack_host_info_cache_path();
    const std::int64_t now = host_info_internal::current_epoch_seconds();
    if (const std::optional<HostInfoSnapshot> cached = read_host_info_snapshot_file(cachePath); cached.has_value()) {
        if (!is_host_info_snapshot_expired(cached.value(), now)) {
            g_cached_snapshot = std::make_shared<HostInfoSnapshot>(cached.value());
            return g_cached_snapshot;
        }
    }

    std::string refreshReason = "missing-cache";
    if (std::filesystem::exists(cachePath)) {
        if (const std::optional<HostInfoSnapshot> cached = read_host_info_snapshot_file(cachePath); cached.has_value()) {
            refreshReason = is_host_info_snapshot_expired(cached.value(), now) ? "expired-ttl" : "manual-live-probe";
        } else {
            refreshReason = "parse-failure";
        }
    }

    HostInfoSnapshot live = host_info_internal::collect_live_snapshot(refreshReason);
    (void)write_host_info_snapshot_file(cachePath, live);
    g_cached_snapshot = std::make_shared<HostInfoSnapshot>(std::move(live));
    return g_cached_snapshot;
}

std::shared_ptr<const HostInfoSnapshot> HostInfoService::refreshSnapshot() {
    std::scoped_lock lock(g_host_info_mutex);
    HostInfoSnapshot live = host_info_internal::collect_live_snapshot("manual-live-probe");
    (void)write_host_info_snapshot_file(default_reqpack_host_info_cache_path(), live);
    g_cached_snapshot = std::make_shared<HostInfoSnapshot>(std::move(live));
    return g_cached_snapshot;
}

bool HostInfoService::invalidateCache() {
    std::scoped_lock lock(g_host_info_mutex);
    g_cached_snapshot.reset();

    std::error_code error;
    const std::filesystem::path cachePath = default_reqpack_host_info_cache_path();
    std::filesystem::remove(cachePath, error);
    return !error;
}
