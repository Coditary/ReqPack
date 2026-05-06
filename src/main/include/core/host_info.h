#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

constexpr int HOST_INFO_CACHE_SCHEMA_VERSION = 1;
constexpr std::int64_t HOST_INFO_CACHE_TTL_SECONDS = 60 * 60 * 24;

struct HostPlatformInfo {
    std::string osFamily{};
    std::string arch{};
    std::string target{};
    std::string supportLevel{"native"};
    std::optional<std::string> supportReason{};
};

struct HostOsInfo {
    std::string family{};
    std::string id{};
    std::string name{};
    std::optional<std::string> version{};
    std::optional<std::string> versionId{};
    std::optional<std::string> prettyName{};
    std::optional<std::string> distroId{};
    std::optional<std::string> distroName{};
};

struct HostKernelInfo {
    std::optional<std::string> name{};
    std::optional<std::string> release{};
    std::optional<std::string> version{};
};

struct HostCpuInfo {
    std::string arch{};
    std::optional<std::string> vendor{};
    std::optional<std::string> model{};
    std::optional<std::uint32_t> logicalCores{};
    std::optional<std::uint32_t> physicalCores{};
};

struct HostMemoryInfo {
    std::optional<std::uint64_t> totalBytes{};
    std::optional<std::uint64_t> availableBytes{};
};

struct HostGpuInfo {
    std::optional<std::string> vendor{};
    std::optional<std::string> model{};
    std::optional<std::string> driverVersion{};
    std::optional<std::string> backend{};
};

struct HostMountInfo {
    std::optional<std::string> device{};
    std::string mountPoint{};
    std::optional<std::string> fsType{};
    std::optional<std::uint64_t> totalBytes{};
    std::optional<std::uint64_t> usedBytes{};
    std::optional<std::uint64_t> availableBytes{};
    std::optional<bool> readOnly{};
};

struct HostStorageInfo {
    std::vector<HostMountInfo> mounts{};
};

struct HostCacheMetadata {
    int schemaVersion{HOST_INFO_CACHE_SCHEMA_VERSION};
    std::int64_t collectedAtEpoch{0};
    std::int64_t expiresAtEpoch{0};
    std::string refreshReason{};
    std::string source{};
};

struct HostInfoSnapshot {
    HostPlatformInfo platform{};
    HostOsInfo os{};
    HostKernelInfo kernel{};
    HostCpuInfo cpu{};
    HostMemoryInfo memory{};
    std::vector<HostGpuInfo> gpus{};
    HostStorageInfo storage{};
    HostCacheMetadata cache{};
};

std::filesystem::path default_reqpack_host_info_cache_path();
std::string normalize_host_architecture(const std::string& value);
HostOsInfo parse_linux_os_release_content(const std::string& content);
bool is_host_info_snapshot_expired(const HostInfoSnapshot& snapshot, std::int64_t nowEpoch);
bool write_host_info_snapshot_file(const std::filesystem::path& path, const HostInfoSnapshot& snapshot);
std::optional<HostInfoSnapshot> read_host_info_snapshot_file(const std::filesystem::path& path);

class HostInfoService {
public:
    static std::shared_ptr<const HostInfoSnapshot> currentSnapshot();
    static std::shared_ptr<const HostInfoSnapshot> refreshSnapshot();
    static bool invalidateCache();
};
