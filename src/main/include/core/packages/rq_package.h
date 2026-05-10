#pragma once

#include <filesystem>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/config/configuration.h"
#include "core/host/host_info.h"

struct RqBinaryEntry {
    std::string name;
    std::string installPath;
    bool primary{false};
};

struct RqPayloadMetadata {
    std::string path;
    std::string archive;
    std::string compression;
    std::string hashAlgorithm;
    std::string hashFile;
    std::uint64_t sizeCompressed{0};
    std::uint64_t sizeInstalledExpected{0};
};

struct RqMetadata {
    int formatVersion{0};
    std::string name;
    std::string version;
    int release{0};
    int revision{0};
    std::string summary;
    std::string description;
    std::string license;
    std::string architecture;
    std::vector<std::string> systems;
    std::string vendor;
    std::string maintainerEmail;
    std::vector<std::string> tags;
    std::string url;
    std::string homepage;
    std::string sourceUrl;
    std::string packager;
    std::string buildDate;
    std::vector<RqBinaryEntry> binaries;
    std::vector<std::string> depends;
    std::vector<std::string> provides;
    std::vector<std::string> conflicts;
    std::vector<std::string> replaces;
    std::optional<RqPayloadMetadata> payload;
};

struct RqPackageLayout {
    RqMetadata metadata;
    std::map<std::string, std::string> hooks;
    std::string identity;
    std::filesystem::path packagePath;
    std::filesystem::path controlDir;
    std::filesystem::path payloadDir;
    std::filesystem::path workDir;
    std::filesystem::path stateDir;
    std::filesystem::path payloadArchivePath;
    bool hasPayload{false};
};

struct RqStateSource {
    std::string source;
    std::string path;
    std::string repository;
    std::string identity;
};

std::string rq_host_architecture();
std::string rq_package_identity(const RqMetadata& metadata);
bool rq_architecture_matches(const std::string& packageArchitecture, const std::string& hostArchitecture);
std::string rq_normalize_architecture(std::string architecture);
std::vector<std::string> rq_normalize_systems(const std::vector<std::string>& systems);
std::map<std::string, std::vector<std::string>> rq_builtin_system_aliases();
std::map<std::string, std::vector<std::string>> rq_merged_system_aliases(const ReqPackConfig& config);
std::set<std::string> rq_host_system_tokens(const HostInfoSnapshot& snapshot);
bool rq_system_matches(
    const std::vector<std::string>& packageSystems,
    const std::set<std::string>& hostSystems,
    const std::map<std::string, std::vector<std::string>>& aliases
);
std::string rq_join_systems(const std::vector<std::string>& systems);
RqMetadata rq_parse_metadata_json(const std::string& content);
std::string rq_metadata_json(const RqMetadata& metadata);
std::map<std::string, std::string> rq_parse_reqpack_hooks(const std::filesystem::path& reqpackLuaPath);

class RqPackageReader {
public:
    static RqPackageLayout load(
        const std::filesystem::path& packagePath,
        const std::filesystem::path& workRoot,
        const std::filesystem::path& stateRoot,
        const ReqPackConfig& config = default_reqpack_config()
    );
};
