#include "plugins/lua_bridge.h"

#include <filesystem>

#include "core/config/configuration.h"
#include "core/host/host_info.h"
#include "core/plugins/plugin_bundle.h"
#include "plugins/lua_bridge_value_mapper.h"

LuaBridge::LuaBridge(const std::string& scriptPath, const ReqPackConfig& config)
    : m_scriptPath(scriptPath),
      m_config(config),
      m_logger(Logger::instance()),
      m_runtime(),
      m_hostRuntime(m_logger, m_config, m_pluginId, m_pluginDirectory, &m_securityMetadata),
      m_bindings(m_runtime, m_hostRuntime) {
    m_bindings.registerBuiltinTypes();
    m_bindings.registerContextTypes();

    const std::filesystem::path resolvedScriptPath(scriptPath);
    m_pluginDirectory = resolvedScriptPath.parent_path().string();
    m_pluginId = resolvedScriptPath.parent_path().filename().string();
    if (const std::optional<PluginBundleLayout> layout = plugin_bundle_read_directory(resolvedScriptPath.parent_path()); layout.has_value()) {
        if (!layout->metadata.name.empty()) {
            m_pluginId = layout->metadata.name;
        }
    }

    sol::state& lua = m_runtime.state();
    lua["REQPACK_PLUGIN_ID"] = m_pluginId;
    lua["REQPACK_PLUGIN_DIR"] = m_pluginDirectory;
    lua["REQPACK_PLUGIN_SCRIPT"] = m_scriptPath;
    lua["print"] = [this](sol::variadic_args args) {
        std::string message;
        bool first = true;
        for (const sol::object& argument : args) {
            if (!first) {
                message += "\t";
            }
            first = false;
            message += LuaBridgeValueMapper::valueToString(argument);
        }

        m_logger.stdout(message, "lua", m_pluginId);
    };

    m_bindings.registerReqpackNamespace();

    if (!m_runtime.loadScript(m_logger, m_scriptPath)) {
        return;
    }

    if (const sol::protected_function getName = m_runtime.pluginFunction("getName"); getName.valid()) {
        auto result = getName();
        if (result.valid() && result.return_count() > 0) {
            m_name = result.get<std::string>();
        }
    }

    if (const sol::protected_function getVersion = m_runtime.pluginFunction("getVersion"); getVersion.valid()) {
        auto result = getVersion();
        if (result.valid() && result.return_count() > 0) {
            m_version = result.get<std::string>();
        }
    }

    if (const sol::protected_function getSecurityMetadata = m_runtime.pluginFunction("getSecurityMetadata"); getSecurityMetadata.valid()) {
        auto result = getSecurityMetadata();
        if (result.valid() && result.return_count() > 0) {
            m_securityMetadata = LuaBridgeValueMapper::pluginSecurityMetadataFromObject(result.get<sol::object>());
        }
    }

    m_fileExtensions = LuaBridgeValueMapper::fileExtensionsFromPluginTable(m_runtime.pluginTable());

    if (m_name.empty()) {
        m_name = m_pluginId;
    }
    if (m_version.empty()) {
        m_version = "0.0.0";
    }
}

PluginCallContext LuaBridge::makeContext(const std::vector<std::string>& flags) const {
    return PluginCallContext{
        .pluginId = m_pluginId,
        .pluginDirectory = m_pluginDirectory,
        .scriptPath = m_scriptPath,
        .flags = flags,
        .host = const_cast<LuaBridge*>(this),
        .proxy = proxy_config_for_system(m_config, m_pluginId),
        .repositories = repositories_for_ecosystem(m_config, m_pluginId),
        .hostInfo = HostInfoService::currentSnapshot()
    };
}

bool LuaBridge::init() {
    if (!m_runtime.validatePluginContract(m_logger, m_pluginId)) {
        return false;
    }

    sol::protected_function luaInit = m_runtime.pluginFunction("init");
    if (luaInit.valid()) {
        auto result = luaInit();
        if (!result.valid()) {
            sol::error err = result;
            log_lua_error(m_logger, m_pluginId, std::string("[Lua Exec Error] init(): ") + err.what());
            return false;
        }
        return result.return_count() == 0 ? true : result.get<bool>();
    }

    return true;
}

bool LuaBridge::shutdown() {
    sol::protected_function luaShutdown = m_runtime.pluginFunction("shutdown");
    const bool shutdownOk = luaShutdown.valid() ? [&]() {
        auto result = luaShutdown();
        return result.valid() ? (result.return_count() == 0 ? true : result.get<bool>()) : false;
    }() : true;

    m_hostRuntime.cleanupAfterShutdown();
    return shutdownOk;
}
