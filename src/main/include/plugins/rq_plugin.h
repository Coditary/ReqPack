#pragma once

#include "plugins/iplugin.h"

#include "core/config/configuration.h"
#include "core/packages/rq_package.h"
#include "core/packages/rq_repository.h"
#include "core/state/rqp_state_store.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

class RqPlugin final : public IPlugin {
public:
    explicit RqPlugin(const ReqPackConfig& config = default_reqpack_config());

    bool init() override;
    bool shutdown() override;

    std::string getName() const override;
    std::string getVersion() const override;
    std::string getPluginId() const override;
    std::string getPluginDirectory() const override;
    std::string getScriptPath() const override;
    std::string getBootstrapPath() const override;
    IPluginRuntimeHost* getRuntimeHost() override;
    bool supportsResolvePackage() const override;

    std::vector<Package> getRequirements() override;
    std::vector<std::string> getCategories() override;
    std::vector<std::string> getFileExtensions() const override;
    std::vector<Package> getMissingPackages(const std::vector<Package>& packages) override;

    bool install(const PluginCallContext& context, const std::vector<Package>& packages) override;
    bool installLocal(const PluginCallContext& context, const std::string& path) override;
    bool remove(const PluginCallContext& context, const std::vector<Package>& packages) override;
    bool update(const PluginCallContext& context, const std::vector<Package>& packages) override;

    std::vector<PackageInfo> list(const PluginCallContext& context) override;
    std::vector<PackageInfo> outdated(const PluginCallContext& context) override;
    std::vector<PackageInfo> search(const PluginCallContext& context, const std::string& prompt) override;
    PackageInfo info(const PluginCallContext& context, const std::string& packageName) override;
    std::optional<Package> resolvePackage(const PluginCallContext& context, const Package& package) override;
    std::vector<PluginEventRecord> takeRecentEvents() override;

private:
    struct ManifestEntry {
        std::string type;
        std::string path;
    };

    ReqPackConfig config_{};
    std::vector<PluginEventRecord> recentEvents_{};
    mutable std::vector<ManifestEntry> pendingManifest_{};
    bool installPackagePath(
        const PluginCallContext& context,
        const std::filesystem::path& path,
        const std::string& sourceType,
        const std::string& sourceValue,
        const std::string& repository = {}
    );
    bool installResolvedPackage(const PluginCallContext& context, const Package& package, const RqRepositoryPackage& resolvedPackage);
    bool removeInstalledPackage(const PluginCallContext& context, const RqpInstalledPackage& installed);
    bool runHook(const PluginCallContext& context, const RqPackageLayout& layout, const std::string& hookKey) const;
    bool runInstalledHook(const PluginCallContext& context, const RqpInstalledPackage& installed, const std::string& hookKey) const;
    bool persistInstalledState(
        const RqPackageLayout& layout,
        const std::string& sourceType,
        const std::string& sourceValue,
        const std::string& repository = {}
    ) const;
    bool removeManifestArtifacts(const RqpInstalledPackage& installed) const;
    static std::vector<ManifestEntry> parseManifestJson(const std::string& content);
    static std::string manifestJson(const std::vector<ManifestEntry>& manifest);
    static PackageInfo packageInfoFromInstalled(const RqpInstalledPackage& installed);
    static std::string installedVersionString(const RqMetadata& metadata);
    static int compareInstalledVersions(const RqpInstalledPackage& left, const RqpInstalledPackage& right);
    static bool repositoryCandidateIsNewer(const RqpInstalledPackage& installed, const RqRepositoryPackage& candidate);
    std::vector<RqRepositoryIndex> loadRepositoryIndexes(const PluginCallContext& context) const;
    static std::string readTextFile(const std::filesystem::path& path);
    static std::string sha256Hex(const std::string& bytes);
    static std::filesystem::path localPathForUrl(const std::string& url);
    static std::filesystem::path downloadPackageArtifact(const PluginCallContext& context, const std::string& url);
    bool initialized_{false};
};
