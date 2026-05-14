#include "plugins/rqp_plugin.h"

#include "rqp_plugin_internal.h"

#include "core/common/build_info.h"

RqpPlugin::RqpPlugin(const ReqPackConfig& config) : config_(config) {
    rqp_plugin_configure_runtime_host(&config_);
}

bool RqpPlugin::init() {
    initialized_ = true;
    return true;
}

bool RqpPlugin::shutdown() {
    initialized_ = false;
    return true;
}

std::string RqpPlugin::getName() const {
    return "ReqPack Native Package Manager";
}

std::string RqpPlugin::getVersion() const {
    return reqpack_build_release_id();
}

std::string RqpPlugin::getPluginId() const {
    return "rqp";
}

std::string RqpPlugin::getPluginDirectory() const {
    return {};
}

std::string RqpPlugin::getScriptPath() const {
    return {};
}

IPluginRuntimeHost* RqpPlugin::getRuntimeHost() {
    return rqp_plugin_runtime_host();
}

bool RqpPlugin::supportsResolvePackage() const {
    return true;
}

bool RqpPlugin::supportsPack() const {
    return true;
}

std::vector<Package> RqpPlugin::getRequirements() {
    return {};
}

std::vector<std::string> RqpPlugin::getCategories() {
    return {"ReqPack", "Native", "Built-in"};
}

std::vector<std::string> RqpPlugin::getFileExtensions() const {
    return {".rqp"};
}

std::vector<PluginEventRecord> RqpPlugin::takeRecentEvents() {
    std::vector<PluginEventRecord> events = std::move(recentEvents_);
    recentEvents_.clear();
    return events;
}

std::vector<std::string> RqpPlugin::takeRecentArtifacts() {
    std::vector<std::string> artifacts = std::move(recentArtifacts_);
    recentArtifacts_.clear();
    return artifacts;
}
