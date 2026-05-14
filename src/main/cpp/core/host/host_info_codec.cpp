#include "host_info_internal.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace {

using boost::property_tree::ptree;
using host_info_internal::optional_trimmed;
using host_info_internal::to_lower_copy;
using host_info_internal::trim_copy;

std::optional<std::uint64_t> parse_u64(const std::string& value) {
    const std::string trimmed = trim_copy(value);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    try {
        return static_cast<std::uint64_t>(std::stoull(trimmed));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::uint32_t> parse_u32(const std::string& value) {
    if (const std::optional<std::uint64_t> parsed = parse_u64(value); parsed.has_value()) {
        return static_cast<std::uint32_t>(parsed.value());
    }
    return std::nullopt;
}

std::optional<bool> parse_bool_optional(const ptree& tree, const std::string& key) {
    const auto raw = tree.get_optional<std::string>(key);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    const std::string normalized = to_lower_copy(trim_copy(raw.value()));
    if (normalized == "true" || normalized == "1") {
        return true;
    }
    if (normalized == "false" || normalized == "0") {
        return false;
    }
    return std::nullopt;
}

void put_optional_string(ptree& tree, const std::string& key, const std::optional<std::string>& value) {
    if (value.has_value()) {
        tree.put(key, value.value());
    }
}

void put_optional_u64(ptree& tree, const std::string& key, const std::optional<std::uint64_t>& value) {
    if (value.has_value()) {
        tree.put(key, value.value());
    }
}

void put_optional_u32(ptree& tree, const std::string& key, const std::optional<std::uint32_t>& value) {
    if (value.has_value()) {
        tree.put(key, value.value());
    }
}

void put_optional_bool(ptree& tree, const std::string& key, const std::optional<bool>& value) {
    if (value.has_value()) {
        tree.put(key, value.value() ? "true" : "false");
    }
}

ptree gpu_to_ptree(const HostGpuInfo& gpu) {
    ptree tree;
    put_optional_string(tree, "vendor", gpu.vendor);
    put_optional_string(tree, "model", gpu.model);
    put_optional_string(tree, "driverVersion", gpu.driverVersion);
    put_optional_string(tree, "backend", gpu.backend);
    return tree;
}

ptree mount_to_ptree(const HostMountInfo& mount) {
    ptree tree;
    put_optional_string(tree, "device", mount.device);
    tree.put("mountPoint", mount.mountPoint);
    put_optional_string(tree, "fsType", mount.fsType);
    put_optional_u64(tree, "totalBytes", mount.totalBytes);
    put_optional_u64(tree, "usedBytes", mount.usedBytes);
    put_optional_u64(tree, "availableBytes", mount.availableBytes);
    put_optional_bool(tree, "readOnly", mount.readOnly);
    return tree;
}

ptree snapshot_to_ptree(const HostInfoSnapshot& snapshot) {
    ptree root;
    root.put("schemaVersion", snapshot.cache.schemaVersion);
    root.put("collectedAtEpoch", snapshot.cache.collectedAtEpoch);
    root.put("expiresAtEpoch", snapshot.cache.expiresAtEpoch);
    root.put("refreshReason", snapshot.cache.refreshReason);

    ptree platform;
    platform.put("osFamily", snapshot.platform.osFamily);
    platform.put("arch", snapshot.platform.arch);
    platform.put("target", snapshot.platform.target);
    platform.put("supportLevel", snapshot.platform.supportLevel);
    put_optional_string(platform, "supportReason", snapshot.platform.supportReason);
    root.add_child("platform", platform);

    ptree os;
    os.put("family", snapshot.os.family);
    os.put("id", snapshot.os.id);
    os.put("name", snapshot.os.name);
    put_optional_string(os, "version", snapshot.os.version);
    put_optional_string(os, "versionId", snapshot.os.versionId);
    put_optional_string(os, "prettyName", snapshot.os.prettyName);
    put_optional_string(os, "distroId", snapshot.os.distroId);
    put_optional_string(os, "distroName", snapshot.os.distroName);
    root.add_child("os", os);

    ptree kernel;
    put_optional_string(kernel, "name", snapshot.kernel.name);
    put_optional_string(kernel, "release", snapshot.kernel.release);
    put_optional_string(kernel, "version", snapshot.kernel.version);
    root.add_child("kernel", kernel);

    ptree cpu;
    cpu.put("arch", snapshot.cpu.arch);
    put_optional_string(cpu, "vendor", snapshot.cpu.vendor);
    put_optional_string(cpu, "model", snapshot.cpu.model);
    put_optional_u32(cpu, "logicalCores", snapshot.cpu.logicalCores);
    put_optional_u32(cpu, "physicalCores", snapshot.cpu.physicalCores);
    root.add_child("cpu", cpu);

    ptree memory;
    put_optional_u64(memory, "totalBytes", snapshot.memory.totalBytes);
    put_optional_u64(memory, "availableBytes", snapshot.memory.availableBytes);
    root.add_child("memory", memory);

    ptree gpus;
    for (const HostGpuInfo& gpu : snapshot.gpus) {
        gpus.push_back(std::make_pair("", gpu_to_ptree(gpu)));
    }
    root.add_child("gpus", gpus);

    ptree storage;
    ptree mounts;
    for (const HostMountInfo& mount : snapshot.storage.mounts) {
        mounts.push_back(std::make_pair("", mount_to_ptree(mount)));
    }
    storage.add_child("mounts", mounts);
    root.add_child("storage", storage);

    return root;
}

std::optional<std::string> optional_tree_string(const ptree& tree, const std::string& key) {
    if (const auto value = tree.get_optional<std::string>(key); value.has_value()) {
        return optional_trimmed(value.value());
    }
    return std::nullopt;
}

std::optional<std::uint64_t> optional_tree_u64(const ptree& tree, const std::string& key) {
    const auto value = tree.get_optional<std::string>(key);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return parse_u64(value.value());
}

std::optional<std::uint32_t> optional_tree_u32(const ptree& tree, const std::string& key) {
    const auto value = tree.get_optional<std::string>(key);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return parse_u32(value.value());
}

std::optional<HostGpuInfo> gpu_from_ptree(const ptree& tree) {
    HostGpuInfo gpu;
    gpu.vendor = optional_tree_string(tree, "vendor");
    gpu.model = optional_tree_string(tree, "model");
    gpu.driverVersion = optional_tree_string(tree, "driverVersion");
    gpu.backend = optional_tree_string(tree, "backend");
    if (!gpu.vendor.has_value() && !gpu.model.has_value() && !gpu.driverVersion.has_value() && !gpu.backend.has_value()) {
        return std::nullopt;
    }
    return gpu;
}

std::optional<HostMountInfo> mount_from_ptree(const ptree& tree) {
    const std::optional<std::string> mountPoint = optional_tree_string(tree, "mountPoint");
    if (!mountPoint.has_value()) {
        return std::nullopt;
    }
    HostMountInfo mount;
    mount.mountPoint = mountPoint.value();
    mount.device = optional_tree_string(tree, "device");
    mount.fsType = optional_tree_string(tree, "fsType");
    mount.totalBytes = optional_tree_u64(tree, "totalBytes");
    mount.usedBytes = optional_tree_u64(tree, "usedBytes");
    mount.availableBytes = optional_tree_u64(tree, "availableBytes");
    mount.readOnly = parse_bool_optional(tree, "readOnly");
    return mount;
}

std::optional<HostInfoSnapshot> snapshot_from_ptree(const ptree& tree) {
    const int schemaVersion = tree.get<int>("schemaVersion", 0);
    if (schemaVersion != HOST_INFO_CACHE_SCHEMA_VERSION) {
        return std::nullopt;
    }

    HostInfoSnapshot snapshot;
    snapshot.cache.schemaVersion = schemaVersion;
    snapshot.cache.collectedAtEpoch = tree.get<std::int64_t>("collectedAtEpoch", 0);
    snapshot.cache.expiresAtEpoch = tree.get<std::int64_t>("expiresAtEpoch", 0);
    snapshot.cache.refreshReason = trim_copy(tree.get<std::string>("refreshReason", {}));
    snapshot.cache.source = "cache";

    if (const auto platformNode = tree.get_child_optional("platform"); platformNode.has_value()) {
        snapshot.platform.osFamily = trim_copy(platformNode->get<std::string>("osFamily", {}));
        snapshot.platform.arch = trim_copy(platformNode->get<std::string>("arch", {}));
        snapshot.platform.target = trim_copy(platformNode->get<std::string>("target", {}));
        snapshot.platform.supportLevel = trim_copy(platformNode->get<std::string>("supportLevel", "native"));
        snapshot.platform.supportReason = optional_tree_string(platformNode.value(), "supportReason");
    }

    if (const auto osNode = tree.get_child_optional("os"); osNode.has_value()) {
        snapshot.os.family = trim_copy(osNode->get<std::string>("family", {}));
        snapshot.os.id = trim_copy(osNode->get<std::string>("id", {}));
        snapshot.os.name = trim_copy(osNode->get<std::string>("name", {}));
        snapshot.os.version = optional_tree_string(osNode.value(), "version");
        snapshot.os.versionId = optional_tree_string(osNode.value(), "versionId");
        snapshot.os.prettyName = optional_tree_string(osNode.value(), "prettyName");
        snapshot.os.distroId = optional_tree_string(osNode.value(), "distroId");
        snapshot.os.distroName = optional_tree_string(osNode.value(), "distroName");
    }

    if (const auto kernelNode = tree.get_child_optional("kernel"); kernelNode.has_value()) {
        snapshot.kernel.name = optional_tree_string(kernelNode.value(), "name");
        snapshot.kernel.release = optional_tree_string(kernelNode.value(), "release");
        snapshot.kernel.version = optional_tree_string(kernelNode.value(), "version");
    }

    if (const auto cpuNode = tree.get_child_optional("cpu"); cpuNode.has_value()) {
        snapshot.cpu.arch = trim_copy(cpuNode->get<std::string>("arch", {}));
        snapshot.cpu.vendor = optional_tree_string(cpuNode.value(), "vendor");
        snapshot.cpu.model = optional_tree_string(cpuNode.value(), "model");
        snapshot.cpu.logicalCores = optional_tree_u32(cpuNode.value(), "logicalCores");
        snapshot.cpu.physicalCores = optional_tree_u32(cpuNode.value(), "physicalCores");
    }

    if (const auto memoryNode = tree.get_child_optional("memory"); memoryNode.has_value()) {
        snapshot.memory.totalBytes = optional_tree_u64(memoryNode.value(), "totalBytes");
        snapshot.memory.availableBytes = optional_tree_u64(memoryNode.value(), "availableBytes");
    }

    if (const auto gpusNode = tree.get_child_optional("gpus"); gpusNode.has_value()) {
        for (const auto& [_, child] : gpusNode.value()) {
            if (const std::optional<HostGpuInfo> gpu = gpu_from_ptree(child); gpu.has_value()) {
                snapshot.gpus.push_back(gpu.value());
            }
        }
    }

    if (const auto storageNode = tree.get_child_optional("storage.mounts"); storageNode.has_value()) {
        for (const auto& [_, child] : storageNode.value()) {
            if (const std::optional<HostMountInfo> mount = mount_from_ptree(child); mount.has_value()) {
                snapshot.storage.mounts.push_back(mount.value());
            }
        }
    }

    if (snapshot.platform.osFamily.empty() || snapshot.platform.arch.empty()) {
        return std::nullopt;
    }

    return snapshot;
}

std::map<std::string, std::string> parse_key_value_lines(const std::string& content, char separator = '=') {
    std::map<std::string, std::string> values;
    std::istringstream stream(content);
    for (std::string line; std::getline(stream, line);) {
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        const std::size_t pos = trimmed.find(separator);
        if (pos == std::string::npos) {
            continue;
        }

        std::string key = trim_copy(trimmed.substr(0, pos));
        std::string value = trim_copy(trimmed.substr(pos + 1));
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        if (!key.empty()) {
            values[key] = value;
        }
    }
    return values;
}

}  // namespace

std::string normalize_host_architecture(const std::string& value) {
    const std::string normalized = to_lower_copy(trim_copy(value));
    if (normalized == "x86_64" || normalized == "amd64") {
        return "x86_64";
    }
    if (normalized == "aarch64" || normalized == "arm64") {
        return "aarch64";
    }
    if (normalized == "x86" || normalized == "i386" || normalized == "i486" || normalized == "i586" || normalized == "i686") {
        return "x86";
    }
    if (normalized == "armv7l" || normalized == "armv7") {
        return "armv7";
    }
    if (normalized == "riscv64") {
        return "riscv64";
    }
    return normalized;
}

HostOsInfo parse_linux_os_release_content(const std::string& content) {
    const std::map<std::string, std::string> values = parse_key_value_lines(content);

    HostOsInfo info;
    info.family = "linux";
    info.id = to_lower_copy(trim_copy(values.contains("ID") ? values.at("ID") : "linux"));
    if (info.id.empty()) {
        info.id = "linux";
    }

    const std::string rawName = trim_copy(values.contains("NAME") ? values.at("NAME") : "Linux");
    info.name = rawName.empty() ? "Linux" : rawName;
    info.version = optional_trimmed(values.contains("VERSION") ? values.at("VERSION") : "");
    info.versionId = optional_trimmed(values.contains("VERSION_ID") ? values.at("VERSION_ID") : "");
    info.prettyName = optional_trimmed(values.contains("PRETTY_NAME") ? values.at("PRETTY_NAME") : "");
    info.distroId = info.id;
    info.distroName = info.name;
    return info;
}

bool is_host_info_snapshot_expired(const HostInfoSnapshot& snapshot, const std::int64_t nowEpoch) {
    return snapshot.cache.expiresAtEpoch <= 0 || snapshot.cache.expiresAtEpoch <= nowEpoch;
}

bool write_host_info_snapshot_file(const std::filesystem::path& path, const HostInfoSnapshot& snapshot) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }

    try {
        boost::property_tree::write_json(output, snapshot_to_ptree(snapshot), false);
    } catch (...) {
        return false;
    }
    return output.good();
}

std::optional<HostInfoSnapshot> read_host_info_snapshot_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return std::nullopt;
    }

    ptree tree;
    try {
        boost::property_tree::read_json(input, tree);
    } catch (...) {
        return std::nullopt;
    }
    return snapshot_from_ptree(tree);
}
