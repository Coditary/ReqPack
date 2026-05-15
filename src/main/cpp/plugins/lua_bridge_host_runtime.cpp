#include "plugins/lua_bridge_host_runtime.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace {

constexpr const char* SILENT_RUNTIME_FLAG = "__reqpack-internal-silent-runtime";

}  // namespace

LuaBridgeHostRuntime::LuaBridgeHostRuntime(Logger& logger,
                                           const ReqPackConfig& config,
                                           const std::string& pluginId,
                                           const std::string& pluginDirectory,
                                           const std::optional<PluginSecurityMetadata>* securityMetadata)
    : m_logger(logger),
      m_config(config),
      m_pluginId(pluginId),
      m_pluginDirectory(pluginDirectory),
      m_securityMetadata(securityMetadata) {}

std::uint64_t LuaBridgeHostRuntime::retainRuntimeBindingContext(const PluginCallContext& context) {
    const std::uint64_t contextId = ++m_nextRuntimeBindingContextId;
    m_runtimeBindingContexts[contextId] = LuaBridgeRuntimeBindingContext{
        .host = context.host,
        .pluginId = context.pluginId,
        .sourceId = context.currentItemId.empty() ? context.pluginId : context.currentItemId,
        .flags = context.flags,
    };
    return contextId;
}

const LuaBridgeRuntimeBindingContext* LuaBridgeHostRuntime::runtimeBindingContext(const std::uint64_t contextId) const {
    const auto it = m_runtimeBindingContexts.find(contextId);
    return it == m_runtimeBindingContexts.end() ? nullptr : &it->second;
}

bool LuaBridgeHostRuntime::hasSilentRuntimeFlag(const std::vector<std::string>& flags) const {
    return std::find(flags.begin(), flags.end(), SILENT_RUNTIME_FLAG) != flags.end();
}

void LuaBridgeHostRuntime::setSilentRuntimeOutput(const bool silent) {
    m_silentRuntimeOutput.store(silent);
}

bool LuaBridgeHostRuntime::silentRuntimeOutput() const {
    return m_silentRuntimeOutput.load();
}

void LuaBridgeHostRuntime::clearRecentEvents() {
    m_recentEvents.clear();
}

void LuaBridgeHostRuntime::clearRecentArtifacts() {
    m_recentArtifacts.clear();
}

std::vector<PluginEventRecord> LuaBridgeHostRuntime::takeRecentEvents() {
    std::vector<PluginEventRecord> events = std::move(m_recentEvents);
    m_recentEvents.clear();
    return events;
}

std::vector<std::string> LuaBridgeHostRuntime::takeRecentArtifacts() {
    std::vector<std::string> artifacts = std::move(m_recentArtifacts);
    m_recentArtifacts.clear();
    return artifacts;
}

void LuaBridgeHostRuntime::beginPackRuntime(const std::string& projectPath, const std::string& outputPath) {
    auto addRuntimeRoot = [&](std::filesystem::path path) {
        if (path.empty()) {
            return;
        }
        std::error_code error;
        if (path.is_relative()) {
            path = std::filesystem::absolute(path, error);
        }
        if (!error) {
            m_runtimeWriteRoots.push_back(path.lexically_normal());
        }
    };

    m_runtimeWriteRoots.clear();
    addRuntimeRoot(std::filesystem::path(projectPath));
    if (!outputPath.empty()) {
        const std::filesystem::path output(outputPath);
        addRuntimeRoot(output.parent_path().empty() ? std::filesystem::current_path() : output.parent_path());
    }
}

void LuaBridgeHostRuntime::endPackRuntime() {
    m_silentRuntimeOutput.store(false);
    m_runtimeWriteRoots.clear();
}

void LuaBridgeHostRuntime::cleanupAfterShutdown() {
    for (const std::string& path : m_tempDirectories) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    m_tempDirectories.clear();
    m_recentArtifacts.clear();
    m_runtimeBindingContexts.clear();
    m_runtimeWriteRoots.clear();
}

std::string LuaBridgeHostRuntime::createTempDirectory(const std::string& pluginId) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / ("reqpack-" + pluginId + "-XXXXXX");
    std::string templateString = tempDir.string();
    std::vector<char> buffer(templateString.begin(), templateString.end());
    buffer.push_back('\0');
    char* created = ::mkdtemp(buffer.data());
    if (created == nullptr) {
        return {};
    }
    m_tempDirectories.emplace_back(created);
    if (!m_runtimeWriteRoots.empty()) {
        m_runtimeWriteRoots.emplace_back(std::filesystem::path(created).lexically_normal());
    }
    return created;
}
