#pragma once

#include "plugins/iplugin.h"

#include "core/configuration.h"
#include "core/rq_package.h"

#include <optional>
#include <string>
#include <vector>

class RqPlugin final : public IPlugin {
public:
    explicit RqPlugin(const ReqPackConfig& config = DEFAULT_REQPACK_CONFIG);

    bool init() override;
    bool shutdown() override;

    std::string getName() const override;
    std::string getVersion() const override;
    std::string getPluginId() const override;
    std::string getPluginDirectory() const override;
    std::string getScriptPath() const override;
    std::string getBootstrapPath() const override;
    IPluginRuntimeHost* getRuntimeHost() override;

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

private:
    ReqPackConfig config_{};
    bool runHook(const PluginCallContext& context, const RqPackageLayout& layout, const std::string& hookKey) const;
    bool persistInstalledState(const RqPackageLayout& layout) const;
    bool initialized_{false};
};
