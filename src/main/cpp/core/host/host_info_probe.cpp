#include "host_info_internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

#include <sys/statvfs.h>
#include <sys/utsname.h>

#if defined(__linux__)
#include <mntent.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/mount.h>
#include <sys/sysctl.h>
#endif

namespace host_info_internal {

std::string trim_copy(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::optional<std::string> optional_trimmed(const std::string& value) {
    const std::string trimmed = trim_copy(value);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    return trimmed;
}

std::int64_t current_epoch_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace host_info_internal

namespace {

using host_info_internal::current_epoch_seconds;
using host_info_internal::optional_trimmed;
using host_info_internal::to_lower_copy;
using host_info_internal::trim_copy;

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::optional<std::string> exec_read_first_line(const std::string& command) {
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return std::nullopt;
    }

    std::string output;
    std::array<char, 512> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output.append(buffer.data());
    }
    const int status = ::pclose(pipe);
    if (status != 0) {
        return std::nullopt;
    }

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (const std::optional<std::string> trimmed = optional_trimmed(line); trimmed.has_value()) {
            return trimmed;
        }
    }
    return std::nullopt;
}

HostPlatformInfo detect_platform_info() {
    HostPlatformInfo platform;
#if defined(__APPLE__)
    platform.osFamily = "macos";
#elif defined(_WIN32)
    platform.osFamily = "windows";
#elif defined(__linux__)
    platform.osFamily = "linux";
#else
    platform.osFamily = "unknown";
#endif

    struct utsname uts{};
    if (::uname(&uts) == 0) {
        platform.arch = normalize_host_architecture(uts.machine);
    }
    if (platform.arch.empty()) {
        platform.arch = normalize_host_architecture(exec_read_first_line("uname -m 2>/dev/null").value_or(std::string{}));
    }
    if (platform.arch.empty()) {
        platform.arch = "unknown";
    }

    std::string targetOs = platform.osFamily;
    if (platform.osFamily == "macos") {
        targetOs = "darwin";
    }
    if (!platform.arch.empty() && !targetOs.empty() && targetOs != "unknown") {
        platform.target = platform.arch + "-" + targetOs;
    }

#if defined(_WIN32)
    platform.supportLevel = "stub";
    platform.supportReason = std::string{"windows host collection not implemented yet"};
#else
    platform.supportLevel = "native";
#endif
    return platform;
}

void fill_uname_fields(HostKernelInfo& kernel, HostCpuInfo& cpu) {
    struct utsname uts{};
    if (::uname(&uts) != 0) {
        return;
    }

    kernel.name = trim_copy(uts.sysname);
    kernel.release = trim_copy(uts.release);
    kernel.version = trim_copy(uts.version);
    if (cpu.arch.empty()) {
        cpu.arch = normalize_host_architecture(uts.machine);
    }
}

void fill_linux_os_info(HostInfoSnapshot& snapshot) {
    const std::string content = read_text_file("/etc/os-release");
    if (content.empty()) {
        snapshot.os.family = "linux";
        snapshot.os.id = "linux";
        snapshot.os.name = "Linux";
        snapshot.os.distroId = std::string{"linux"};
        snapshot.os.distroName = std::string{"Linux"};
        return;
    }
    snapshot.os = parse_linux_os_release_content(content);
}

void fill_macos_os_info(HostInfoSnapshot& snapshot) {
    snapshot.os.family = "macos";
    snapshot.os.id = "macos";
    snapshot.os.name = "macOS";
    snapshot.os.distroName = std::string{"macOS"};

    if (const std::optional<std::string> name = exec_read_first_line("sw_vers -productName 2>/dev/null"); name.has_value()) {
        snapshot.os.name = name.value();
        snapshot.os.distroName = name.value();
    }
    if (const std::optional<std::string> version = exec_read_first_line("sw_vers -productVersion 2>/dev/null"); version.has_value()) {
        snapshot.os.version = version;
        snapshot.os.versionId = version;
    }
    if (snapshot.os.version.has_value()) {
        snapshot.os.prettyName = snapshot.os.name + " " + snapshot.os.version.value();
    } else {
        snapshot.os.prettyName = snapshot.os.name;
    }
}

void fill_windows_os_info(HostInfoSnapshot& snapshot) {
    snapshot.os.family = "windows";
    snapshot.os.id = "windows";
    snapshot.os.name = "Windows";
    snapshot.os.prettyName = std::string{"Windows"};
}

void fill_os_info(HostInfoSnapshot& snapshot) {
#if defined(__APPLE__)
    fill_macos_os_info(snapshot);
#elif defined(_WIN32)
    fill_windows_os_info(snapshot);
#elif defined(__linux__)
    fill_linux_os_info(snapshot);
#else
    snapshot.os.family = snapshot.platform.osFamily;
    snapshot.os.id = snapshot.platform.osFamily;
    snapshot.os.name = snapshot.platform.osFamily.empty() ? "Unknown" : snapshot.platform.osFamily;
#endif
}

void fill_cpu_info_linux(HostCpuInfo& cpu) {
    const std::string content = read_text_file("/proc/cpuinfo");
    if (content.empty()) {
        return;
    }

    std::istringstream stream(content);
    std::string line;
    std::map<std::string, std::string> firstProcessorFields;
    std::vector<std::pair<std::string, std::string>> coreTuples;
    std::string physicalId;
    std::string coreId;
    bool firstBlock = true;

    auto flush_block = [&]() {
        if (!physicalId.empty() || !coreId.empty()) {
            coreTuples.emplace_back(physicalId, coreId);
        }
        physicalId.clear();
        coreId.clear();
    };

    while (std::getline(stream, line)) {
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty()) {
            flush_block();
            firstBlock = false;
            continue;
        }

        const std::size_t pos = trimmed.find(':');
        if (pos == std::string::npos) {
            continue;
        }

        const std::string key = trim_copy(trimmed.substr(0, pos));
        const std::string value = trim_copy(trimmed.substr(pos + 1));
        if (firstBlock && !key.empty() && !value.empty()) {
            firstProcessorFields.emplace(key, value);
        }
        if (key == "vendor_id") {
            cpu.vendor = value;
        } else if (key == "model name") {
            cpu.model = value;
        } else if (key == "physical id") {
            physicalId = value;
        } else if (key == "core id") {
            coreId = value;
        }
    }
    flush_block();

    if (!cpu.vendor.has_value()) {
        cpu.vendor = optional_trimmed(firstProcessorFields["vendor_id"]);
    }
    if (!cpu.model.has_value()) {
        cpu.model = optional_trimmed(firstProcessorFields["model name"]);
    }

    std::sort(coreTuples.begin(), coreTuples.end());
    coreTuples.erase(std::unique(coreTuples.begin(), coreTuples.end()), coreTuples.end());
    if (!coreTuples.empty()) {
        cpu.physicalCores = static_cast<std::uint32_t>(coreTuples.size());
    }
}

#if defined(__APPLE__)
std::optional<std::string> sysctl_string(const char* name) {
    size_t size = 0;
    if (::sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
        return std::nullopt;
    }
    std::string value(size, '\0');
    if (::sysctlbyname(name, value.data(), &size, nullptr, 0) != 0 || size == 0) {
        return std::nullopt;
    }
    value.resize(size > 0 && value.back() == '\0' ? size - 1 : size);
    return optional_trimmed(value);
}

template <typename T>
std::optional<T> sysctl_scalar(const char* name) {
    T value{};
    size_t size = sizeof(value);
    if (::sysctlbyname(name, &value, &size, nullptr, 0) != 0 || size != sizeof(value)) {
        return std::nullopt;
    }
    return value;
}
#endif

void fill_cpu_info(HostInfoSnapshot& snapshot) {
    snapshot.cpu.arch = snapshot.platform.arch;
    fill_uname_fields(snapshot.kernel, snapshot.cpu);

    const unsigned int logical = std::thread::hardware_concurrency();
    if (logical > 0) {
        snapshot.cpu.logicalCores = logical;
    }

#if defined(__linux__)
    fill_cpu_info_linux(snapshot.cpu);
#elif defined(__APPLE__)
    if (const std::optional<std::string> vendor = sysctl_string("machdep.cpu.vendor"); vendor.has_value()) {
        snapshot.cpu.vendor = vendor;
    }
    if (const std::optional<std::string> model = sysctl_string("machdep.cpu.brand_string"); model.has_value()) {
        snapshot.cpu.model = model;
    }
    if (const std::optional<std::uint32_t> physical = sysctl_scalar<std::uint32_t>("hw.physicalcpu"); physical.has_value()) {
        snapshot.cpu.physicalCores = physical;
    }
    if (const std::optional<std::uint32_t> logicalCpu = sysctl_scalar<std::uint32_t>("hw.logicalcpu"); logicalCpu.has_value()) {
        snapshot.cpu.logicalCores = logicalCpu;
    }
#endif
}

void fill_memory_info_linux(HostMemoryInfo& memory) {
    const std::string content = read_text_file("/proc/meminfo");
    if (content.empty()) {
        return;
    }

    std::istringstream stream(content);
    for (std::string line; std::getline(stream, line);) {
        const std::size_t pos = line.find(':');
        if (pos == std::string::npos) {
            continue;
        }
        const std::string key = trim_copy(line.substr(0, pos));
        const std::string rest = trim_copy(line.substr(pos + 1));
        std::istringstream valueStream(rest);
        std::uint64_t amount = 0;
        std::string unit;
        valueStream >> amount >> unit;
        if (amount == 0) {
            continue;
        }
        const std::uint64_t bytes = unit == "kB" ? amount * 1024ULL : amount;
        if (key == "MemTotal") {
            memory.totalBytes = bytes;
        } else if (key == "MemAvailable") {
            memory.availableBytes = bytes;
        }
    }
}

void fill_memory_info(HostMemoryInfo& memory) {
#if defined(__linux__)
    fill_memory_info_linux(memory);
#elif defined(__APPLE__)
    if (const std::optional<std::uint64_t> total = sysctl_scalar<std::uint64_t>("hw.memsize"); total.has_value()) {
        memory.totalBytes = total;
    }
#endif
}

bool should_skip_mount_type(const std::string& fsType) {
    static const std::array<const char*, 15> ignored{
        "proc", "sysfs", "tmpfs", "devtmpfs", "devfs", "cgroup", "cgroup2", "overlay",
        "squashfs", "autofs", "debugfs", "tracefs", "nsfs", "mqueue", "fusectl"
    };
    return std::find(ignored.begin(), ignored.end(), fsType) != ignored.end();
}

void fill_mount_capacity(const std::string& mountPoint, HostMountInfo& mount) {
    struct statvfs info{};
    if (::statvfs(mountPoint.c_str(), &info) != 0) {
        return;
    }

    const std::uint64_t total = static_cast<std::uint64_t>(info.f_blocks) * static_cast<std::uint64_t>(info.f_frsize);
    const std::uint64_t available = static_cast<std::uint64_t>(info.f_bavail) * static_cast<std::uint64_t>(info.f_frsize);
    const std::uint64_t freeBytes = static_cast<std::uint64_t>(info.f_bfree) * static_cast<std::uint64_t>(info.f_frsize);
    mount.totalBytes = total;
    mount.availableBytes = available;
    if (total >= freeBytes) {
        mount.usedBytes = total - freeBytes;
    }
    mount.readOnly = (info.f_flag & ST_RDONLY) != 0;
}

#if defined(__linux__)
void fill_mounts_linux(HostStorageInfo& storage) {
    FILE* mounts = ::setmntent("/proc/mounts", "r");
    if (mounts == nullptr) {
        return;
    }

    while (mntent* entry = ::getmntent(mounts)) {
        const std::string mountPoint = trim_copy(entry->mnt_dir != nullptr ? entry->mnt_dir : "");
        const std::string fsType = trim_copy(entry->mnt_type != nullptr ? entry->mnt_type : "");
        if (mountPoint.empty() || should_skip_mount_type(fsType)) {
            continue;
        }

        HostMountInfo mount;
        if (entry->mnt_fsname != nullptr) {
            mount.device = optional_trimmed(entry->mnt_fsname);
        }
        mount.mountPoint = mountPoint;
        mount.fsType = optional_trimmed(fsType);
        fill_mount_capacity(mountPoint, mount);
        storage.mounts.push_back(std::move(mount));
    }
    ::endmntent(mounts);
}
#endif

#if defined(__APPLE__)
void fill_mounts_macos(HostStorageInfo& storage) {
    struct statfs* mounts = nullptr;
    const int count = ::getmntinfo(&mounts, MNT_NOWAIT);
    if (count <= 0 || mounts == nullptr) {
        return;
    }

    for (int index = 0; index < count; ++index) {
        const std::string mountPoint = trim_copy(mounts[index].f_mntonname);
        const std::string fsType = trim_copy(mounts[index].f_fstypename);
        if (mountPoint.empty() || should_skip_mount_type(fsType)) {
            continue;
        }

        HostMountInfo mount;
        mount.mountPoint = mountPoint;
        mount.device = optional_trimmed(mounts[index].f_mntfromname);
        mount.fsType = optional_trimmed(fsType);
        fill_mount_capacity(mountPoint, mount);
        storage.mounts.push_back(std::move(mount));
    }
}
#endif

void fill_storage_info(HostStorageInfo& storage) {
#if defined(__linux__)
    fill_mounts_linux(storage);
#elif defined(__APPLE__)
    fill_mounts_macos(storage);
#endif
}

void try_add_linux_gpu_from_lspci(std::vector<HostGpuInfo>& gpus) {
    FILE* pipe = ::popen("lspci 2>/dev/null", "r");
    if (pipe == nullptr) {
        return;
    }

    std::array<char, 1024> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        const std::string line = trim_copy(buffer.data());
        const std::string lower = to_lower_copy(line);
        if (lower.find("vga compatible controller") == std::string::npos &&
            lower.find("3d controller") == std::string::npos &&
            lower.find("display controller") == std::string::npos) {
            continue;
        }

        HostGpuInfo gpu;
        gpu.backend = std::string{"lspci"};
        if (lower.find("nvidia") != std::string::npos) {
            gpu.vendor = std::string{"NVIDIA"};
        } else if (lower.find("amd") != std::string::npos || lower.find("advanced micro devices") != std::string::npos || lower.find("ati") != std::string::npos) {
            gpu.vendor = std::string{"AMD"};
        } else if (lower.find("intel") != std::string::npos) {
            gpu.vendor = std::string{"Intel"};
        }
        gpu.model = line;
        gpus.push_back(std::move(gpu));
    }
    ::pclose(pipe);
}

void try_add_linux_gpu_from_nvidia_smi(std::vector<HostGpuInfo>& gpus) {
    FILE* pipe = ::popen("nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null", "r");
    if (pipe == nullptr) {
        return;
    }

    std::array<char, 512> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        const std::string line = trim_copy(buffer.data());
        if (line.empty()) {
            continue;
        }
        HostGpuInfo gpu;
        gpu.vendor = std::string{"NVIDIA"};
        gpu.backend = std::string{"nvidia-smi"};
        const std::size_t comma = line.find(',');
        if (comma == std::string::npos) {
            gpu.model = line;
        } else {
            gpu.model = trim_copy(line.substr(0, comma));
            gpu.driverVersion = optional_trimmed(line.substr(comma + 1));
        }
        gpus.push_back(std::move(gpu));
    }
    ::pclose(pipe);
}

void fill_gpu_info(std::vector<HostGpuInfo>& gpus) {
#if defined(__linux__)
    try_add_linux_gpu_from_nvidia_smi(gpus);
    if (gpus.empty()) {
        try_add_linux_gpu_from_lspci(gpus);
    }
#elif defined(__APPLE__)
    FILE* pipe = ::popen("system_profiler SPDisplaysDataType 2>/dev/null", "r");
    if (pipe == nullptr) {
        return;
    }

    HostGpuInfo current;
    current.backend = std::string{"system_profiler"};
    bool hasCurrent = false;
    std::array<char, 1024> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        const std::string line = trim_copy(buffer.data());
        if (line.rfind("Chipset Model:", 0) == 0) {
            if (hasCurrent && current.model.has_value()) {
                gpus.push_back(current);
            }
            current = HostGpuInfo{};
            current.backend = std::string{"system_profiler"};
            current.model = optional_trimmed(line.substr(std::string{"Chipset Model:"}.size()));
            hasCurrent = true;
        } else if (line.rfind("Vendor:", 0) == 0) {
            current.vendor = optional_trimmed(line.substr(std::string{"Vendor:"}.size()));
        }
    }
    if (hasCurrent && current.model.has_value()) {
        gpus.push_back(current);
    }
    ::pclose(pipe);
#endif
}

}  // namespace

namespace host_info_internal {

HostInfoSnapshot collect_live_snapshot(const std::string& refreshReason) {
    HostInfoSnapshot snapshot;
    snapshot.platform = detect_platform_info();
    snapshot.cache.schemaVersion = HOST_INFO_CACHE_SCHEMA_VERSION;
    snapshot.cache.collectedAtEpoch = current_epoch_seconds();
    snapshot.cache.expiresAtEpoch = snapshot.cache.collectedAtEpoch + HOST_INFO_CACHE_TTL_SECONDS;
    snapshot.cache.refreshReason = refreshReason;
    snapshot.cache.source = "live";

    fill_os_info(snapshot);
    fill_cpu_info(snapshot);
    fill_memory_info(snapshot.memory);
    fill_storage_info(snapshot.storage);
    fill_gpu_info(snapshot.gpus);

    if (snapshot.os.family.empty()) {
        snapshot.os.family = snapshot.platform.osFamily;
    }
    if (snapshot.os.id.empty()) {
        snapshot.os.id = snapshot.platform.osFamily;
    }
    if (snapshot.os.name.empty()) {
        snapshot.os.name = snapshot.platform.osFamily.empty() ? "Unknown" : snapshot.platform.osFamily;
    }
    if (snapshot.cpu.arch.empty()) {
        snapshot.cpu.arch = snapshot.platform.arch;
    }
    return snapshot;
}

}  // namespace host_info_internal
