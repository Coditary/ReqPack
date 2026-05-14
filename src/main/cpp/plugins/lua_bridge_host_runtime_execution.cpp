#include "plugins/lua_bridge_host_runtime.h"

#include "core/archive/archive_resolver.h"
#include "core/download/downloader.h"
#include "plugins/exec_rules.h"
#include "plugins/lua_bridge_execution_policy.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <utility>

namespace {

ArchiveExtractionOptions archive_options_from_config(const ReqPackConfig& config) {
    return ArchiveExtractionOptions{
        .password = resolve_archive_password(config),
        .interactive = config.interaction.interactive,
    };
}

}  // namespace

bool LuaBridgeHostRuntime::shouldEnforceExecutionPolicy() const {
    return m_config.security.requireThinLayer && m_config.execution.checkVirtualFileSystemWrite;
}

ExecResult LuaBridgeHostRuntime::denyExecution(const std::string& message) const {
    if (!m_silentRuntimeOutput.load()) {
        m_logger.emit(OutputAction::LOG, OutputContext{
            .level = spdlog::level::err,
            .message = message,
            .source = "plugin",
            .scope = m_pluginId,
        });
    }
    return ExecResult{.success = false, .exitCode = 126, .stdoutText = {}, .stderrText = message};
}

ExecResult LuaBridgeHostRuntime::runCommand(const std::string& command) const {
    return executeCommandWithPolicy(m_pluginId, command, m_silentRuntimeOutput.load());
}

ExecResult LuaBridgeHostRuntime::executeCommandWithPolicy(const std::string& sourceId, const std::string& command, const bool silent) const {
    if (m_execOverride) {
        return m_execOverride(sourceId, command);
    }
    if (!shouldEnforceExecutionPolicy()) {
        return run_plugin_command(m_logger, sourceId, m_pluginId, command, silent);
    }

    const PluginSecurityMetadata metadata = m_securityMetadata->value_or(PluginSecurityMetadata{});
    if (const std::optional<std::string> error = LuaBridgeExecutionPolicy::validate(metadata, m_pluginId, m_pluginDirectory, command, m_runtimeWriteRoots); error.has_value()) {
        return denyExecution(error.value());
    }

    return run_plugin_command(m_logger, sourceId, m_pluginId, command, silent);
}

ExecResult LuaBridgeHostRuntime::executeCommandWithPolicy(const std::string& sourceId,
                                                          const std::string& command,
                                                          const sol::object& rules,
                                                          const bool silent) const {
    if (m_execOverride) {
        return m_execOverride(sourceId, command);
    }
    if (!shouldEnforceExecutionPolicy()) {
        return run_plugin_command(m_logger, sourceId, m_pluginId, command, rules, silent);
    }

    const PluginSecurityMetadata metadata = m_securityMetadata->value_or(PluginSecurityMetadata{});
    if (const std::optional<std::string> error = LuaBridgeExecutionPolicy::validate(metadata, m_pluginId, m_pluginDirectory, command, m_runtimeWriteRoots); error.has_value()) {
        return denyExecution(error.value());
    }

    return run_plugin_command(m_logger, sourceId, m_pluginId, command, rules, silent);
}

DownloadResult LuaBridgeHostRuntime::downloadToPath(const std::string& url, const std::string& destinationPath) {
    Downloader downloader(nullptr, m_config);
    const std::filesystem::path sourcePath = [url]() {
        if (url.rfind("file://", 0) == 0) {
            return std::filesystem::path(url.substr(7));
        }
        return std::filesystem::path(url);
    }();

    const std::string suffix = generic_archive_suffix(sourcePath);
    const std::string wrapper = archive_wrapper_suffix(sourcePath);
    std::string extension;
    if (!wrapper.empty()) {
        const std::string innerSuffix = generic_archive_suffix(sourcePath.stem());
        extension = innerSuffix.empty() ? wrapper : innerSuffix + wrapper;
    } else {
        extension = suffix;
    }
    const std::filesystem::path targetPath = extension.empty() ? std::filesystem::path(destinationPath)
                                                                : std::filesystem::path(destinationPath + extension);
    if (!downloader.download(url, targetPath.string())) {
        return {};
    }

    DownloadResult result;
    result.success = true;
    result.resolvedPath = destinationPath;

    try {
        if (extract_archive_in_place(targetPath, archive_options_from_config(m_config))) {
            if (targetPath != std::filesystem::path(destinationPath)) {
                std::error_code error;
                std::filesystem::remove_all(destinationPath, error);
                std::filesystem::rename(targetPath, destinationPath, error);
                if (error) {
                    return {};
                }
            }
            result.resolvedPath = destinationPath;
        } else if (targetPath != std::filesystem::path(destinationPath)) {
            std::error_code error;
            std::filesystem::rename(targetPath, destinationPath, error);
            if (error) {
                return {};
            }
        }
    } catch (...) {
        return {};
    }

    return result;
}

ExecResult LuaBridgeHostRuntime::execute(const std::string& pluginId, const std::string& command) const {
    return executeCommandWithPolicy(pluginId, command, m_silentRuntimeOutput.load());
}

DownloadResult LuaBridgeHostRuntime::download(const std::string& pluginId, const std::string& url, const std::string& destinationPath) {
    (void)pluginId;
    return downloadToPath(url, destinationPath);
}

void LuaBridgeHostRuntime::setExecOverride(ExecOverride execOverride) {
    m_execOverride = std::move(execOverride);
}
