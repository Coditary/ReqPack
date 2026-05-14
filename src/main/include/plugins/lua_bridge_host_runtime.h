#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <sol/sol.hpp>

#include "core/config/configuration.h"
#include "output/logger.h"
#include "plugins/iplugin.h"

struct LuaBridgeRuntimeBindingContext {
    IPluginRuntimeHost* host{nullptr};
    std::string pluginId;
    std::string sourceId;
    std::vector<std::string> flags;
};

class LuaBridgeHostRuntime {
public:
    using ExecOverride = std::function<ExecResult(const std::string& sourceId, const std::string& command)>;

    LuaBridgeHostRuntime(Logger& logger,
                         const ReqPackConfig& config,
                         const std::string& pluginId,
                         const std::string& pluginDirectory,
                         const std::optional<PluginSecurityMetadata>* securityMetadata);

    std::uint64_t retainRuntimeBindingContext(const PluginCallContext& context);
    const LuaBridgeRuntimeBindingContext* runtimeBindingContext(std::uint64_t contextId) const;

    bool hasSilentRuntimeFlag(const std::vector<std::string>& flags) const;
    void setSilentRuntimeOutput(bool silent);
    bool silentRuntimeOutput() const;

    void clearRecentEvents();
    void clearRecentArtifacts();
    std::vector<PluginEventRecord> takeRecentEvents();
    std::vector<std::string> takeRecentArtifacts();

    void beginPackRuntime(const std::string& projectPath, const std::string& outputPath);
    void endPackRuntime();
    void cleanupAfterShutdown();

    ExecResult runCommand(const std::string& command) const;
    ExecResult executeCommandWithPolicy(const std::string& sourceId, const std::string& command, bool silent) const;
    ExecResult executeCommandWithPolicy(const std::string& sourceId,
                                        const std::string& command,
                                        const sol::object& rules,
                                        bool silent) const;

    DownloadResult downloadToPath(const std::string& url, const std::string& destinationPath);

    void logDebug(const std::string& pluginId, const std::string& message);
    void logInfo(const std::string& pluginId, const std::string& message);
    void logWarn(const std::string& pluginId, const std::string& message);
    void logError(const std::string& pluginId, const std::string& message);
    void emitStatus(const std::string& pluginId, int statusCode);
    void emitProgress(const std::string& pluginId, const DisplayProgressMetrics& metrics);
    void emitBeginStep(const std::string& pluginId, const std::string& label);
    void emitCommit(const std::string& pluginId);
    void emitSuccess(const std::string& pluginId);
    void emitFailure(const std::string& pluginId, const std::string& message);
    void emitEvent(const std::string& pluginId, const std::string& eventName, const std::string& payload);
    void registerArtifact(const std::string& pluginId, const std::string& payload);
    ExecResult execute(const std::string& pluginId, const std::string& command) const;
    std::string createTempDirectory(const std::string& pluginId);
    DownloadResult download(const std::string& pluginId, const std::string& url, const std::string& destinationPath);

    void setExecOverride(ExecOverride execOverride);

private:
    bool shouldEnforceExecutionPolicy() const;
    ExecResult denyExecution(const std::string& message) const;

    Logger& m_logger;
    const ReqPackConfig& m_config;
    const std::string& m_pluginId;
    const std::string& m_pluginDirectory;
    const std::optional<PluginSecurityMetadata>* m_securityMetadata;
    mutable std::atomic<bool> m_silentRuntimeOutput{false};
    ExecOverride m_execOverride;
    std::vector<std::string> m_tempDirectories;
    std::vector<PluginEventRecord> m_recentEvents;
    std::vector<std::string> m_recentArtifacts;
    std::vector<std::filesystem::path> m_runtimeWriteRoots;
    std::unordered_map<std::uint64_t, LuaBridgeRuntimeBindingContext> m_runtimeBindingContexts;
    std::uint64_t m_nextRuntimeBindingContextId{0};
};
