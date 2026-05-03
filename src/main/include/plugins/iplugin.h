#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "core/configuration.h"
#include "core/types.h"
#include "core/version_compare.h"

#define REQPACK_API_VERSION 4

struct ExecResult {
	bool success{false};
	int exitCode{1};
	std::string stdoutText;
	std::string stderrText;
};

struct DownloadResult {
	bool success{false};
	std::string resolvedPath;
};

struct PluginSecurityMetadata {
	std::string osvEcosystem{};
	std::string purlType{};
	VersionComparatorSpec versionComparator{};
};

struct PluginEventRecord {
	std::string name;
	std::string payload;
};

class IPluginRuntimeHost {
public:
	virtual ~IPluginRuntimeHost() = default;

	virtual void logDebug(const std::string& pluginId, const std::string& message) = 0;
	virtual void logInfo(const std::string& pluginId, const std::string& message) = 0;
	virtual void logWarn(const std::string& pluginId, const std::string& message) = 0;
	virtual void logError(const std::string& pluginId, const std::string& message) = 0;
	virtual void emitStatus(const std::string& pluginId, int statusCode) = 0;
	virtual void emitProgress(const std::string& pluginId, int percent) = 0;
	virtual void emitBeginStep(const std::string& pluginId, const std::string& label) = 0;
	virtual void emitCommit(const std::string& pluginId) = 0;
	virtual void emitSuccess(const std::string& pluginId) = 0;
	virtual void emitFailure(const std::string& pluginId, const std::string& message) = 0;
	virtual void emitEvent(const std::string& pluginId, const std::string& eventName, const std::string& payload) = 0;
	virtual void registerArtifact(const std::string& pluginId, const std::string& payload) = 0;
	virtual ExecResult execute(const std::string& pluginId, const std::string& command) = 0;
	virtual std::string createTempDirectory(const std::string& pluginId) = 0;
	virtual DownloadResult download(const std::string& pluginId, const std::string& url, const std::string& destinationPath) = 0;
};

struct PluginCallContext {
	std::string pluginId;
	std::string pluginDirectory;
	std::string scriptPath;
	std::string bootstrapPath;
	std::vector<std::string> flags;
	IPluginRuntimeHost* host{nullptr};
	std::optional<ProxyConfig> proxy;
	/// Item id used by the display layer (e.g. "dnf:python").
	/// When non-empty, passed as the first arg to IPluginRuntimeHost callbacks
	/// so the display can correlate events to the right item row.
	std::string currentItemId{};
	std::vector<RepositoryEntry> repositories{};

	ExecResult execute(const std::string& command) const {
		return host != nullptr ? host->execute(pluginId, command) : ExecResult{};
	}

	std::string createTempDirectory() const {
		return host != nullptr ? host->createTempDirectory(pluginId) : std::string{};
	}

	DownloadResult downloadFile(const std::string& url, const std::string& destinationPath) const {
		return host != nullptr ? host->download(pluginId, url, destinationPath) : DownloadResult{};
	}

	void logDebug(const std::string& message) const {
		if (host != nullptr) {
			host->logDebug(pluginId, message);
		}
	}

	void logInfo(const std::string& message) const {
		if (host != nullptr) {
			host->logInfo(pluginId, message);
		}
	}

	void logWarn(const std::string& message) const {
		if (host != nullptr) {
			host->logWarn(pluginId, message);
		}
	}

	void logError(const std::string& message) const {
		if (host != nullptr) {
			host->logError(pluginId, message);
		}
	}

	void emitStatus(int statusCode) const {
		if (host != nullptr) {
			host->emitStatus(currentItemId.empty() ? pluginId : currentItemId, statusCode);
		}
	}

	void emitProgress(int percent) const {
		if (host != nullptr) {
			host->emitProgress(currentItemId.empty() ? pluginId : currentItemId, percent);
		}
	}

	void emitBeginStep(const std::string& label) const {
		if (host != nullptr) {
			host->emitBeginStep(currentItemId.empty() ? pluginId : currentItemId, label);
		}
	}

	void emitCommit() const {
		if (host != nullptr) {
			host->emitCommit(currentItemId.empty() ? pluginId : currentItemId);
		}
	}

	void emitSuccess() const {
		if (host != nullptr) {
			host->emitSuccess(currentItemId.empty() ? pluginId : currentItemId);
		}
	}

	void emitFailure(const std::string& message) const {
		if (host != nullptr) {
			host->emitFailure(currentItemId.empty() ? pluginId : currentItemId, message);
		}
	}

	void emitEvent(const std::string& eventName, const std::string& payload) const {
		if (host != nullptr) {
			host->emitEvent(currentItemId.empty() ? pluginId : currentItemId, eventName, payload);
		}
	}

	void registerArtifact(const std::string& payload) const {
		if (host != nullptr) {
			host->registerArtifact(pluginId, payload);
		}
	}
};

class IPlugin {
public:
    virtual ~IPlugin() = default;
	virtual uint32_t getInterfaceVersion() const {
        return REQPACK_API_VERSION;
    }

	virtual bool init() = 0;
	virtual bool shutdown() = 0;

    virtual std::string getName() const = 0;
    virtual std::string getVersion() const = 0;
	virtual std::string getPluginId() const = 0;
	virtual std::string getPluginDirectory() const = 0;
	virtual std::string getScriptPath() const = 0;
	virtual std::string getBootstrapPath() const = 0;
	virtual IPluginRuntimeHost* getRuntimeHost() = 0;
	virtual std::optional<PluginSecurityMetadata> getSecurityMetadata() const {
		return std::nullopt;
	}
    
	virtual std::vector<Package> getRequirements() = 0;
	virtual std::vector<std::string> getCategories() = 0;
	virtual std::vector<std::string> getFileExtensions() const { return {}; }
	virtual std::vector<Package> getMissingPackages(const std::vector<Package>& packages) = 0;
	virtual bool supportsProxyResolution() const {
		return false;
	}
	virtual bool supportsResolvePackage() const {
		return false;
	}
	virtual std::vector<PluginEventRecord> takeRecentEvents() {
		return {};
	}

    virtual bool install(const PluginCallContext& context, const std::vector<Package>& packages) = 0;
    virtual bool installLocal(const PluginCallContext& context, const std::string& path) = 0;
    virtual bool remove(const PluginCallContext& context, const std::vector<Package>& packages) = 0;
    virtual bool update(const PluginCallContext& context, const std::vector<Package>& packages) = 0;

    virtual std::vector<PackageInfo> list(const PluginCallContext& context) = 0;

    virtual std::vector<PackageInfo> outdated(const PluginCallContext& context) = 0;
    
    virtual std::vector<PackageInfo> search(const PluginCallContext& context, const std::string& prompt) = 0;
    
    virtual PackageInfo info(const PluginCallContext& context, const std::string& packageName) = 0;

    virtual std::optional<Package> resolvePackage(const PluginCallContext& context, const Package& package) {
        (void)context;
        (void)package;
        return std::nullopt;
    }

	virtual std::optional<ProxyResolution> resolveProxyRequest(const PluginCallContext& context, const Request& request) {
		(void)context;
		(void)request;
		return std::nullopt;
	}
};
