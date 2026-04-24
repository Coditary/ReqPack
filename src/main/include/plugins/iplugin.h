#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include "core/types.h"

#define REQPACK_API_VERSION 3

struct ExecResult {
	bool success{false};
	int exitCode{1};
	std::string stdoutText;
	std::string stderrText;
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
	virtual bool download(const std::string& pluginId, const std::string& url, const std::string& destinationPath) = 0;
};

struct PluginCallContext {
	std::string pluginId;
	std::string pluginDirectory;
	std::string scriptPath;
	std::string bootstrapPath;
	std::vector<std::string> flags;
	IPluginRuntimeHost* host{nullptr};

	ExecResult execute(const std::string& command) const {
		return host != nullptr ? host->execute(pluginId, command) : ExecResult{};
	}

	std::string createTempDirectory() const {
		return host != nullptr ? host->createTempDirectory(pluginId) : std::string{};
	}

	bool downloadFile(const std::string& url, const std::string& destinationPath) const {
		return host != nullptr ? host->download(pluginId, url, destinationPath) : false;
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
			host->emitStatus(pluginId, statusCode);
		}
	}

	void emitProgress(int percent) const {
		if (host != nullptr) {
			host->emitProgress(pluginId, percent);
		}
	}

	void emitBeginStep(const std::string& label) const {
		if (host != nullptr) {
			host->emitBeginStep(pluginId, label);
		}
	}

	void emitCommit() const {
		if (host != nullptr) {
			host->emitCommit(pluginId);
		}
	}

	void emitSuccess() const {
		if (host != nullptr) {
			host->emitSuccess(pluginId);
		}
	}

	void emitFailure(const std::string& message) const {
		if (host != nullptr) {
			host->emitFailure(pluginId, message);
		}
	}

	void emitEvent(const std::string& eventName, const std::string& payload) const {
		if (host != nullptr) {
			host->emitEvent(pluginId, eventName, payload);
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
    
	virtual std::vector<Package> getRequirements() = 0;
	virtual std::vector<std::string> getCategories() = 0;
	virtual std::vector<Package> getMissingPackages(const std::vector<Package>& packages) = 0;

    virtual bool install(const PluginCallContext& context, const std::vector<Package>& packages) = 0;
    virtual bool installLocal(const PluginCallContext& context, const std::string& path) = 0;
    virtual bool remove(const PluginCallContext& context, const std::vector<Package>& packages) = 0;
    virtual bool update(const PluginCallContext& context, const std::vector<Package>& packages) = 0;

    virtual std::vector<PackageInfo> list(const PluginCallContext& context) = 0;
    
    virtual std::vector<PackageInfo> search(const PluginCallContext& context, const std::string& prompt) = 0;
    
    virtual PackageInfo info(const PluginCallContext& context, const std::string& packageName) = 0;
};
