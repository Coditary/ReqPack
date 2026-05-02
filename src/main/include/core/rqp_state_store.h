#pragma once

#include "core/configuration.h"
#include "core/rq_package.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct RqpInstalledPackage {
    RqMetadata metadata;
    RqStateSource source;
    std::map<std::string, std::string> hooks;
    std::string identity;
    std::filesystem::path stateDir;
    std::filesystem::path metadataPath;
    std::filesystem::path reqpackLuaPath;
    std::filesystem::path scriptsDir;
    std::filesystem::path manifestPath;
    std::filesystem::path sourcePath;
};

class RqpStateStore {
public:
    explicit RqpStateStore(const ReqPackConfig& config = DEFAULT_REQPACK_CONFIG);

    std::vector<RqpInstalledPackage> listInstalled() const;
    std::vector<RqpInstalledPackage> findInstalled(const std::string& name, const std::string& version = {}) const;
    std::optional<RqpInstalledPackage> loadInstalled(const std::filesystem::path& stateDir) const;
    bool removeInstalledState(const RqpInstalledPackage& installed) const;

private:
    ReqPackConfig config_{};
    static RqStateSource parseSourceJson(const std::string& content);
    static std::string readTextFile(const std::filesystem::path& path);
};
