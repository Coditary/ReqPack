#include "plugins/lua_bridge.h"

#include "plugins/lua_bridge_value_mapper.h"

std::vector<Package> LuaBridge::getRequirements() {
    if (!m_runtime.hasPluginTable()) {
        return {};
    }
    sol::protected_function func = m_runtime.pluginFunction("getRequirements");
    if (func.valid()) {
        auto result = func();
        if (result.valid() && result.return_count() > 0) {
            if (const sol::object value = result.get<sol::object>(); value.valid()) {
                if (const auto requirements = LuaBridgeValueMapper::packagesFromObject(value)) {
                    return requirements.value();
                }
            }
        }
    }
    return {};
}

std::vector<std::string> LuaBridge::getCategories() {
    if (!m_runtime.hasPluginTable()) {
        return {};
    }
    sol::protected_function func = m_runtime.pluginFunction("getCategories");
    if (func.valid()) {
        auto result = func();
        if (result.valid()) {
            return result.get<std::vector<std::string>>();
        }
    }
    return {};
}

std::vector<Package> LuaBridge::getMissingPackages(const std::vector<Package>& packages) {
    if (!m_runtime.hasPluginTable()) {
        return packages;
    }
    sol::protected_function func = m_runtime.pluginFunction("getMissingPackages");
    if (!func.valid()) {
        log_lua_error(m_logger, m_pluginId, "[Lua API Error] getMissingPackages(packages) is required.");
        return packages;
    }

    auto result = func(packages);
    if (result.valid()) {
        if (result.return_count() == 0) {
            log_lua_error(m_logger, m_pluginId, "[Lua API Error] getMissingPackages(packages) must return a package list.");
            return packages;
        }

        if (const sol::object value = result.get<sol::object>(); value.valid()) {
            if (const auto missingPackages = LuaBridgeValueMapper::packagesFromObject(value)) {
                std::vector<Package> normalizedPackages = missingPackages.value();
                LuaBridgeValueMapper::inheritMissingPackageFields(packages, normalizedPackages);
                return normalizedPackages;
            }
        }

        log_lua_error(m_logger, m_pluginId, "[Lua API Error] getMissingPackages(packages) must return a package list.");
        return packages;
    }

    sol::error err = result;
    log_lua_error(m_logger, m_pluginId, std::string("Lua Error (getMissingPackages): ") + err.what());
    return packages;
}

bool LuaBridge::supportsResolvePackage() const {
    return m_runtime.hasPluginFunction("resolvePackage");
}

bool LuaBridge::supportsPack() const {
    return m_runtime.hasPluginFunction("pack");
}

bool LuaBridge::supportsProxyResolution() const {
    return m_runtime.hasPluginFunction("resolveProxyRequest");
}

std::vector<PluginEventRecord> LuaBridge::takeRecentEvents() {
    return m_hostRuntime.takeRecentEvents();
}

std::vector<std::string> LuaBridge::takeRecentArtifacts() {
    return m_hostRuntime.takeRecentArtifacts();
}

bool LuaBridge::install(const PluginCallContext& context, const std::vector<Package>& packages) {
    m_hostRuntime.clearRecentEvents();
    sol::protected_function func = m_runtime.pluginFunction("install");
    if (!func.valid()) {
        return false;
    }

    auto result = func(context, packages);
    if (!result.valid()) {
        sol::error err = result;
        log_lua_error(m_logger, m_pluginId, std::string("Lua Error (install): ") + err.what());
        return false;
    }
    return result.return_count() == 0 ? true : result.get<bool>();
}

bool LuaBridge::installLocal(const PluginCallContext& context, const std::string& path) {
    m_hostRuntime.clearRecentEvents();
    sol::protected_function func = m_runtime.pluginFunction("installLocal");
    if (!func.valid()) {
        return false;
    }

    auto result = func(context, path);
    if (!result.valid()) {
        sol::error err = result;
        log_lua_error(m_logger, m_pluginId, std::string("Lua Error (installLocal): ") + err.what());
        return false;
    }
    return result.return_count() == 0 ? true : result.get<bool>();
}

bool LuaBridge::remove(const PluginCallContext& context, const std::vector<Package>& packages) {
    m_hostRuntime.clearRecentEvents();
    sol::protected_function func = m_runtime.pluginFunction("remove");
    if (!func.valid()) {
        return false;
    }

    auto result = func(context, packages);
    if (!result.valid()) {
        sol::error err = result;
        log_lua_error(m_logger, m_pluginId, std::string("Lua Error (remove): ") + err.what());
        return false;
    }
    return result.return_count() == 0 ? true : result.get<bool>();
}

bool LuaBridge::update(const PluginCallContext& context, const std::vector<Package>& packages) {
    m_hostRuntime.clearRecentEvents();
    sol::protected_function func = m_runtime.pluginFunction("update");
    if (!func.valid()) {
        return false;
    }

    auto result = func(context, packages);
    if (!result.valid()) {
        sol::error err = result;
        log_lua_error(m_logger, m_pluginId, std::string("Lua Error (update): ") + err.what());
        return false;
    }
    return result.return_count() == 0 ? true : result.get<bool>();
}

bool LuaBridge::pack(const PluginCallContext& context,
                     const std::string& projectPath,
                     const std::string& outputPath,
                     const std::vector<std::string>& flags) {
    m_hostRuntime.clearRecentEvents();
    m_hostRuntime.clearRecentArtifacts();

    sol::protected_function func = m_runtime.pluginFunction("pack");
    if (!func.valid()) {
        return false;
    }

    m_hostRuntime.beginPackRuntime(projectPath, outputPath);
    const bool silentRuntime = m_hostRuntime.hasSilentRuntimeFlag(context.flags) || m_hostRuntime.hasSilentRuntimeFlag(flags);
    m_hostRuntime.setSilentRuntimeOutput(silentRuntime);
    auto result = func(context, projectPath, outputPath, flags);
    m_hostRuntime.endPackRuntime();
    if (!result.valid()) {
        sol::error err = result;
        log_lua_error(m_logger, m_pluginId, std::string("Lua Error (pack): ") + err.what());
        return false;
    }
    return result.return_count() == 0 ? true : result.get<bool>();
}

std::vector<PackageInfo> LuaBridge::list(const PluginCallContext& context) {
    sol::protected_function func = m_runtime.pluginFunction("list");
    if (!func.valid()) {
        return {};
    }

    const bool silentRuntime = m_hostRuntime.hasSilentRuntimeFlag(context.flags);
    m_hostRuntime.setSilentRuntimeOutput(silentRuntime);
    auto result = func(context);
    m_hostRuntime.setSilentRuntimeOutput(false);
    if (!result.valid()) {
        sol::error err = result;
        log_lua_error(m_logger, m_pluginId, std::string("Lua Error (list): ") + err.what());
        return {};
    }
    if (result.return_count() == 0) {
        return {};
    }
    return LuaBridgeValueMapper::packageInfoListFromObject(result.get<sol::object>());
}

std::vector<PackageInfo> LuaBridge::outdated(const PluginCallContext& context) {
    sol::protected_function func = m_runtime.pluginFunction("outdated");
    if (!func.valid()) {
        return {};
    }

    auto result = func(context);
    if (!result.valid()) {
        sol::error err = result;
        log_lua_error(m_logger, m_pluginId, std::string("Lua Error (outdated): ") + err.what());
        return {};
    }
    if (result.return_count() == 0) {
        return {};
    }
    return LuaBridgeValueMapper::packageInfoListFromObject(result.get<sol::object>());
}

std::vector<PackageInfo> LuaBridge::search(const PluginCallContext& context, const std::string& prompt) {
    sol::protected_function func = m_runtime.pluginFunction("search");
    if (!func.valid()) {
        return {};
    }

    auto result = func(context, prompt);
    if (!result.valid()) {
        sol::error err = result;
        log_lua_error(m_logger, m_pluginId, std::string("Lua Error (search): ") + err.what());
        return {};
    }
    if (result.return_count() == 0) {
        return {};
    }
    return LuaBridgeValueMapper::packageInfoListFromObject(result.get<sol::object>());
}

PackageInfo LuaBridge::info(const PluginCallContext& context, const std::string& packageName) {
    sol::protected_function func = m_runtime.pluginFunction("info");
    if (!func.valid()) {
        return {};
    }

    const bool silentRuntime = m_hostRuntime.hasSilentRuntimeFlag(context.flags);
    m_hostRuntime.setSilentRuntimeOutput(silentRuntime);
    auto result = func(context, packageName);
    m_hostRuntime.setSilentRuntimeOutput(false);
    if (!result.valid()) {
        sol::error err = result;
        log_lua_error(m_logger, m_pluginId, std::string("Lua Error (info): ") + err.what());
        return {};
    }
    if (result.return_count() == 0) {
        return {};
    }
    return LuaBridgeValueMapper::packageInfoFromObject(result.get<sol::object>());
}

std::optional<Package> LuaBridge::resolvePackage(const PluginCallContext& context, const Package& package) {
    sol::protected_function func = m_runtime.pluginFunction("resolvePackage");
    if (!func.valid()) {
        return std::nullopt;
    }

    const bool silentRuntime = m_hostRuntime.hasSilentRuntimeFlag(context.flags);
    m_hostRuntime.setSilentRuntimeOutput(silentRuntime);
    auto result = func(context, package);
    m_hostRuntime.setSilentRuntimeOutput(false);
    if (!result.valid()) {
        sol::error err = result;
        log_lua_error(m_logger, m_pluginId, std::string("Lua Error (resolvePackage): ") + err.what());
        return std::nullopt;
    }
    if (result.return_count() == 0) {
        return std::nullopt;
    }

    const sol::object value = result.get<sol::object>();
    if (!value.valid() || value.is<sol::lua_nil_t>()) {
        return std::nullopt;
    }
    if (const auto resolved = LuaBridgeValueMapper::packageFromObject(value)) {
        return resolved.value();
    }

    log_lua_error(m_logger, m_pluginId, "[Lua API Error] resolvePackage(context, package) must return a package or nil.");
    return std::nullopt;
}

std::optional<ProxyResolution> LuaBridge::resolveProxyRequest(const PluginCallContext& context, const Request& request) {
    sol::protected_function func = m_runtime.pluginFunction("resolveProxyRequest");
    if (!func.valid()) {
        return std::nullopt;
    }

    const bool silentRuntime = m_hostRuntime.hasSilentRuntimeFlag(context.flags);
    m_hostRuntime.setSilentRuntimeOutput(silentRuntime);
    auto result = func(context, request);
    m_hostRuntime.setSilentRuntimeOutput(false);
    if (!result.valid()) {
        sol::error err = result;
        log_lua_error(m_logger, m_pluginId, std::string("Lua Error (resolveProxyRequest): ") + err.what());
        return std::nullopt;
    }
    if (result.return_count() == 0) {
        return std::nullopt;
    }

    const sol::object value = result.get<sol::object>();
    if (!value.valid() || value.is<sol::lua_nil_t>()) {
        return std::nullopt;
    }
    if (const auto resolved = LuaBridgeValueMapper::proxyResolutionFromObject(value)) {
        return resolved.value();
    }

    log_lua_error(m_logger, m_pluginId, "[Lua API Error] resolveProxyRequest(context, request) must return a proxy resolution table or nil.");
    return std::nullopt;
}

void LuaBridge::logDebug(const std::string& pluginId, const std::string& message) {
    m_hostRuntime.logDebug(pluginId, message);
}

void LuaBridge::logInfo(const std::string& pluginId, const std::string& message) {
    m_hostRuntime.logInfo(pluginId, message);
}

void LuaBridge::logWarn(const std::string& pluginId, const std::string& message) {
    m_hostRuntime.logWarn(pluginId, message);
}

void LuaBridge::logError(const std::string& pluginId, const std::string& message) {
    m_hostRuntime.logError(pluginId, message);
}

void LuaBridge::emitStatus(const std::string& pluginId, const int statusCode) {
    m_hostRuntime.emitStatus(pluginId, statusCode);
}

void LuaBridge::emitProgress(const std::string& pluginId, const DisplayProgressMetrics& metrics) {
    m_hostRuntime.emitProgress(pluginId, metrics);
}

void LuaBridge::emitBeginStep(const std::string& pluginId, const std::string& label) {
    m_hostRuntime.emitBeginStep(pluginId, label);
}

void LuaBridge::emitCommit(const std::string& pluginId) {
    m_hostRuntime.emitCommit(pluginId);
}

void LuaBridge::emitSuccess(const std::string& pluginId) {
    m_hostRuntime.emitSuccess(pluginId);
}

void LuaBridge::emitFailure(const std::string& pluginId, const std::string& message) {
    m_hostRuntime.emitFailure(pluginId, message);
}

void LuaBridge::emitEvent(const std::string& pluginId, const std::string& eventName, const std::string& payload) {
    m_hostRuntime.emitEvent(pluginId, eventName, payload);
}

void LuaBridge::registerArtifact(const std::string& pluginId, const std::string& payload) {
    m_hostRuntime.registerArtifact(pluginId, payload);
}

ExecResult LuaBridge::execute(const std::string& pluginId, const std::string& command) {
    return m_hostRuntime.execute(pluginId, command);
}

std::string LuaBridge::createTempDirectory(const std::string& pluginId) {
    return m_hostRuntime.createTempDirectory(pluginId);
}

DownloadResult LuaBridge::download(const std::string& pluginId, const std::string& url, const std::string& destinationPath) {
    return m_hostRuntime.download(pluginId, url, destinationPath);
}

void LuaBridge::setExecOverride(ExecOverride execOverride) {
    m_hostRuntime.setExecOverride(std::move(execOverride));
}
