#include "plugins/lua_bridge_host_runtime.h"

#include "output/progress_metrics.h"

#include <string>

namespace {

std::string runtime_output_source(const std::string& pluginId) {
    return pluginId.find(':') != std::string::npos ? pluginId : "plugin";
}

}  // namespace

void LuaBridgeHostRuntime::logDebug(const std::string& pluginId, const std::string& message) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    m_logger.emit(OutputAction::LOG, OutputContext{.level = spdlog::level::debug, .message = message, .source = "plugin", .scope = pluginId});
}

void LuaBridgeHostRuntime::logInfo(const std::string& pluginId, const std::string& message) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    m_logger.emit(OutputAction::LOG, OutputContext{.level = spdlog::level::info, .message = message, .source = "plugin", .scope = pluginId});
}

void LuaBridgeHostRuntime::logWarn(const std::string& pluginId, const std::string& message) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    m_logger.emit(OutputAction::LOG, OutputContext{.level = spdlog::level::warn, .message = message, .source = "plugin", .scope = pluginId});
}

void LuaBridgeHostRuntime::logError(const std::string& pluginId, const std::string& message) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    m_logger.emit(OutputAction::LOG, OutputContext{.level = spdlog::level::err, .message = message, .source = "plugin", .scope = pluginId});
}

void LuaBridgeHostRuntime::emitStatus(const std::string& pluginId, const int statusCode) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    m_logger.emit(OutputAction::PLUGIN_STATUS,
                  OutputContext{.source = runtime_output_source(pluginId), .scope = m_pluginId, .statusCode = statusCode});
}

void LuaBridgeHostRuntime::emitProgress(const std::string& pluginId, const DisplayProgressMetrics& metrics) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    const DisplayProgressMetrics normalized = canonicalize_progress_metrics(metrics);
    if (!normalized.percent.has_value() && !normalized.currentBytes.has_value() && !normalized.totalBytes.has_value() &&
        !normalized.bytesPerSecond.has_value()) {
        return;
    }
    m_logger.emit(OutputAction::PLUGIN_PROGRESS, OutputContext{
        .source = runtime_output_source(pluginId),
        .scope = m_pluginId,
        .progressPercent = normalized.percent,
        .currentBytes = normalized.currentBytes,
        .totalBytes = normalized.totalBytes,
        .bytesPerSecond = normalized.bytesPerSecond,
    });
}

void LuaBridgeHostRuntime::emitBeginStep(const std::string& pluginId, const std::string& label) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    m_logger.emit(OutputAction::PLUGIN_EVENT,
                  OutputContext{.source = runtime_output_source(pluginId), .scope = m_pluginId, .eventName = "begin_step", .payload = label});
}

void LuaBridgeHostRuntime::emitCommit(const std::string& pluginId) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    m_logger.emit(OutputAction::PLUGIN_EVENT,
                  OutputContext{.source = runtime_output_source(pluginId), .scope = m_pluginId, .eventName = "commit", .payload = "committed"});
}

void LuaBridgeHostRuntime::emitSuccess(const std::string& pluginId) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    m_logger.emit(OutputAction::PLUGIN_EVENT,
                  OutputContext{.source = runtime_output_source(pluginId), .scope = m_pluginId, .eventName = "success", .payload = "ok"});
}

void LuaBridgeHostRuntime::emitFailure(const std::string& pluginId, const std::string& message) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    m_logger.emit(OutputAction::PLUGIN_EVENT,
                  OutputContext{.source = runtime_output_source(pluginId), .scope = m_pluginId, .eventName = "failed", .payload = message});
}

void LuaBridgeHostRuntime::emitEvent(const std::string& pluginId, const std::string& eventName, const std::string& payload) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    m_recentEvents.push_back(PluginEventRecord{.name = eventName, .payload = payload});
    m_logger.emit(OutputAction::PLUGIN_EVENT,
                  OutputContext{.source = runtime_output_source(pluginId), .scope = m_pluginId, .eventName = eventName, .payload = payload});
}

void LuaBridgeHostRuntime::registerArtifact(const std::string& pluginId, const std::string& payload) {
    m_recentArtifacts.push_back(payload);
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    m_logger.emit(OutputAction::PLUGIN_ARTIFACT,
                  OutputContext{.source = runtime_output_source(pluginId), .scope = m_pluginId, .payload = payload});
}
