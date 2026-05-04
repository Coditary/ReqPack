#pragma once

#include <sol/sol.hpp>
#include <string>
#include <vector>
#include <memory>
#include <atomic>

#include "core/configuration.h"
#include "core/types.h"
#include "output/logger.h"
#include "plugins/iplugin.h"

class LuaBridge : public IPlugin, public IPluginRuntimeHost {
private:
    sol::state m_lua;
    std::string m_name;
    std::string m_version;
	std::string m_pluginId;
	std::string m_pluginDirectory;
	std::string m_scriptPath;
	std::string m_bootstrapPath;
	std::optional<PluginSecurityMetadata> m_securityMetadata;
	std::vector<std::string> m_fileExtensions;
	Logger& m_logger;
    
    sol::table m_pluginTable;
	ReqPackConfig m_config;
	std::vector<std::string> m_tempDirectories;
	std::vector<PluginEventRecord> m_recentEvents;
	mutable std::atomic<bool> m_silentRuntimeOutput{false};

	PluginCallContext makeContext(const std::vector<std::string>& flags) const;
	void register_context_types();
	void register_reqpack_namespace();
	bool validatePluginContract() const;
	std::vector<PackageInfo> packageInfoListFromObject(const sol::object& value) const;
	PackageInfo packageInfoFromObject(const sol::object& value) const;
	std::string serializeLuaPayload(const sol::object& value) const;
	bool hasSilentRuntimeFlag(const std::vector<std::string>& flags) const;
	ExecResult runCommand(const std::string& command) const;
	ExecResult runCommand(const std::string& command, bool silent) const;
	DownloadResult downloadToPath(const std::string& url, const std::string& destinationPath);

public:
    LuaBridge(const std::string& scriptPath, const ReqPackConfig& config = default_reqpack_config());
    virtual ~LuaBridge() = default;

    bool init() override;
    bool shutdown() override;

    std::string getName() const override { return m_name; }
    std::string getVersion() const override { return m_version; }
	std::string getPluginId() const override { return m_pluginId; }
	std::string getPluginDirectory() const override { return m_pluginDirectory; }
	std::string getScriptPath() const override { return m_scriptPath; }
	std::string getBootstrapPath() const override { return m_bootstrapPath; }
	IPluginRuntimeHost* getRuntimeHost() override { return this; }
	std::optional<PluginSecurityMetadata> getSecurityMetadata() const override { return m_securityMetadata; }
	std::vector<std::string> getFileExtensions() const override { return m_fileExtensions; }
    
	std::vector<Package> getRequirements() override;
    std::vector<std::string> getCategories() override;
    std::vector<Package> getMissingPackages(const std::vector<Package>& packages) override;
	bool supportsProxyResolution() const override;
	bool supportsResolvePackage() const override;
	std::vector<PluginEventRecord> takeRecentEvents() override;

    bool install(const PluginCallContext& context, const std::vector<Package>& packages) override;
    bool installLocal(const PluginCallContext& context, const std::string& path) override;
    bool remove(const PluginCallContext& context, const std::vector<Package>& packages) override;
    bool update(const PluginCallContext& context, const std::vector<Package>& packages) override;

    std::vector<PackageInfo> list(const PluginCallContext& context) override;
    std::vector<PackageInfo> outdated(const PluginCallContext& context) override;
    std::vector<PackageInfo> search(const PluginCallContext& context, const std::string& prompt) override;
    PackageInfo info(const PluginCallContext& context, const std::string& packageName) override;
    std::optional<Package> resolvePackage(const PluginCallContext& context, const Package& package) override;
	std::optional<ProxyResolution> resolveProxyRequest(const PluginCallContext& context, const Request& request) override;

	void logDebug(const std::string& pluginId, const std::string& message) override;
	void logInfo(const std::string& pluginId, const std::string& message) override;
	void logWarn(const std::string& pluginId, const std::string& message) override;
	void logError(const std::string& pluginId, const std::string& message) override;
	void emitStatus(const std::string& pluginId, int statusCode) override;
	void emitProgress(const std::string& pluginId, const DisplayProgressMetrics& metrics) override;
	void emitBeginStep(const std::string& pluginId, const std::string& label) override;
	void emitCommit(const std::string& pluginId) override;
	void emitSuccess(const std::string& pluginId) override;
	void emitFailure(const std::string& pluginId, const std::string& message) override;
	void emitEvent(const std::string& pluginId, const std::string& eventName, const std::string& payload) override;
	void registerArtifact(const std::string& pluginId, const std::string& payload) override;
	ExecResult execute(const std::string& pluginId, const std::string& command) override;
	std::string createTempDirectory(const std::string& pluginId) override;
	DownloadResult download(const std::string& pluginId, const std::string& url, const std::string& destinationPath) override;
};
