#include <catch2/catch.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "core/host/host_info.h"

namespace {

class TempDir {
public:
    explicit TempDir(const std::string& prefix)
        : path_(std::filesystem::temp_directory_path() /
            (prefix + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class ScopedEnvVar {
public:
    ScopedEnvVar(std::string name, std::string value)
        : name_(std::move(name)) {
        if (const char* existing = std::getenv(name_.c_str())) {
            previous_ = std::string(existing);
        }
        ::setenv(name_.c_str(), value.c_str(), 1);
    }

    ~ScopedEnvVar() {
        if (previous_.has_value()) {
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.is_open());
    output << content;
}

HostInfoSnapshot sample_snapshot() {
    HostInfoSnapshot snapshot;
    snapshot.platform.osFamily = "linux";
    snapshot.platform.arch = "x86_64";
    snapshot.platform.target = "x86_64-linux";
    snapshot.platform.supportLevel = "native";
    snapshot.os.family = "linux";
    snapshot.os.id = "fedora";
    snapshot.os.name = "Fedora Linux";
    snapshot.os.version = std::string{"41"};
    snapshot.os.versionId = std::string{"41"};
    snapshot.os.prettyName = std::string{"Fedora Linux 41"};
    snapshot.os.distroId = std::string{"fedora"};
    snapshot.os.distroName = std::string{"Fedora Linux"};
    snapshot.kernel.name = std::string{"Linux"};
    snapshot.kernel.release = std::string{"6.8.0-test"};
    snapshot.cpu.arch = "x86_64";
    snapshot.cpu.vendor = std::string{"GenuineIntel"};
    snapshot.cpu.model = std::string{"Test CPU"};
    snapshot.cpu.logicalCores = 16;
    snapshot.cpu.physicalCores = 8;
    snapshot.memory.totalBytes = 32000000000ULL;
    snapshot.memory.availableBytes = 16000000000ULL;
    snapshot.gpus.push_back(HostGpuInfo{
        .vendor = std::string{"NVIDIA"},
        .model = std::string{"RTX Test"},
        .driverVersion = std::string{"1.2.3"},
        .backend = std::string{"nvidia-smi"},
    });
    snapshot.storage.mounts.push_back(HostMountInfo{
        .device = std::string{"/dev/test0"},
        .mountPoint = "/",
        .fsType = std::string{"ext4"},
        .totalBytes = 1000ULL,
        .usedBytes = 250ULL,
        .availableBytes = 750ULL,
        .readOnly = false,
    });
    snapshot.cache.schemaVersion = HOST_INFO_CACHE_SCHEMA_VERSION;
    snapshot.cache.collectedAtEpoch = 100;
    snapshot.cache.expiresAtEpoch = 200;
    snapshot.cache.refreshReason = "missing-cache";
    snapshot.cache.source = "live";
    return snapshot;
}

}  // namespace

TEST_CASE("host info normalizes common architecture aliases", "[unit][host_info][normalize]") {
    CHECK(normalize_host_architecture("AMD64") == "x86_64");
    CHECK(normalize_host_architecture("arm64") == "aarch64");
    CHECK(normalize_host_architecture("i686") == "x86");
    CHECK(normalize_host_architecture("riscv64") == "riscv64");
}

TEST_CASE("host info parses linux os-release content", "[unit][host_info][parse]") {
    const HostOsInfo info = parse_linux_os_release_content(R"osr(NAME="Fedora Linux"
ID=fedora
VERSION_ID="41"
VERSION="41 (Workstation Edition)"
PRETTY_NAME="Fedora Linux 41 (Workstation Edition)"
)osr");

    CHECK(info.family == "linux");
    CHECK(info.id == "fedora");
    CHECK(info.name == "Fedora Linux");
    REQUIRE(info.version.has_value());
    CHECK(info.version.value() == "41 (Workstation Edition)");
    REQUIRE(info.versionId.has_value());
    CHECK(info.versionId.value() == "41");
    REQUIRE(info.prettyName.has_value());
    CHECK(info.prettyName.value() == "Fedora Linux 41 (Workstation Edition)");
    REQUIRE(info.distroId.has_value());
    CHECK(info.distroId.value() == "fedora");
}

TEST_CASE("host info resolves cache path under reqpack cache root", "[unit][host_info][path]") {
    TempDir tempDir{"reqpack-host-info-path"};
    ScopedEnvVar cacheHome{"XDG_CACHE_HOME", (tempDir.path() / "cache-root").string()};

    CHECK(default_reqpack_host_info_cache_path() == tempDir.path() / "cache-root" / "reqpack" / "host" / "info.v1.json");
}

TEST_CASE("host info cache file roundtrips snapshot", "[unit][host_info][cache]") {
    TempDir tempDir{"reqpack-host-info-roundtrip"};
    const std::filesystem::path path = tempDir.path() / "host.json";
    const HostInfoSnapshot snapshot = sample_snapshot();

    REQUIRE(write_host_info_snapshot_file(path, snapshot));

    const std::optional<HostInfoSnapshot> loaded = read_host_info_snapshot_file(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->platform.osFamily == "linux");
    CHECK(loaded->platform.arch == "x86_64");
    CHECK(loaded->platform.target == "x86_64-linux");
    CHECK(loaded->platform.supportLevel == "native");
    CHECK(loaded->os.id == "fedora");
    REQUIRE(loaded->cpu.logicalCores.has_value());
    CHECK(loaded->cpu.logicalCores.value() == 16);
    REQUIRE(loaded->memory.totalBytes.has_value());
    CHECK(loaded->memory.totalBytes.value() == 32000000000ULL);
    REQUIRE(loaded->gpus.size() == 1);
    REQUIRE(loaded->gpus[0].model.has_value());
    CHECK(loaded->gpus[0].model.value() == "RTX Test");
    REQUIRE(loaded->storage.mounts.size() == 1);
    CHECK(loaded->storage.mounts[0].mountPoint == "/");
    CHECK(loaded->cache.source == "cache");
}

TEST_CASE("host info cache parser rejects invalid json", "[unit][host_info][cache]") {
    TempDir tempDir{"reqpack-host-info-invalid"};
    const std::filesystem::path path = tempDir.path() / "host.json";
    write_file(path, "{ not json }");

    CHECK_FALSE(read_host_info_snapshot_file(path).has_value());
}

TEST_CASE("host info cache expiry uses expiresAtEpoch", "[unit][host_info][cache]") {
    HostInfoSnapshot snapshot = sample_snapshot();
    snapshot.cache.expiresAtEpoch = 500;

    CHECK_FALSE(is_host_info_snapshot_expired(snapshot, 499));
    CHECK(is_host_info_snapshot_expired(snapshot, 500));
    CHECK(is_host_info_snapshot_expired(snapshot, 600));
}
