#include "plugins/lua_bridge_runtime.h"

#include <array>

namespace {

bool execute_file(sol::state& lua, Logger& logger, const std::string& path) {
    sol::load_result loadResult = lua.load_file(path);
    if (!loadResult.valid()) {
        sol::error err = loadResult;
        log_lua_error(logger, "load", path + ": " + err.what());
        return false;
    }

    const sol::protected_function_result executionResult = loadResult();
    if (!executionResult.valid()) {
        sol::error err = executionResult;
        log_lua_error(logger, "exec", path + ": " + err.what());
        return false;
    }

    return true;
}

}  // namespace

void log_lua_error(Logger& logger, const std::string& scope, const std::string& message) {
    logger.emit(OutputAction::LOG, OutputContext{.level = spdlog::level::err, .message = message, .source = "lua", .scope = scope});
}

LuaBridgeScriptRuntime::LuaBridgeScriptRuntime() {
    m_lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math, sol::lib::io);
}

sol::state& LuaBridgeScriptRuntime::state() {
    return m_lua;
}

const sol::state& LuaBridgeScriptRuntime::state() const {
    return m_lua;
}

bool LuaBridgeScriptRuntime::loadScript(Logger& logger, const std::string& path) {
    if (!execute_file(m_lua, logger, path)) {
        return false;
    }
    m_pluginTable = m_lua["plugin"];
    return true;
}

bool LuaBridgeScriptRuntime::validatePluginContract(Logger& logger, const std::string& pluginId) const {
    if (!m_pluginTable.valid()) {
        log_lua_error(logger, pluginId, "[Lua API Error] plugin table is required.");
        return false;
    }

    const std::array<const char*, 10> requiredMethods{
        "getName",
        "getVersion",
        "getRequirements",
        "getCategories",
        "getMissingPackages",
        "install",
        "installLocal",
        "remove",
        "update",
        "list"
    };

    for (const char* method : requiredMethods) {
        sol::protected_function function = m_pluginTable[method];
        if (!function.valid()) {
            log_lua_error(logger, pluginId, std::string("[Lua API Error] ") + method + " is required.");
            return false;
        }
    }

    const std::array<const char*, 2> requiredQueryMethods{"search", "info"};
    for (const char* method : requiredQueryMethods) {
        sol::protected_function function = m_pluginTable[method];
        if (!function.valid()) {
            log_lua_error(logger, pluginId, std::string("[Lua API Error] ") + method + " is required.");
            return false;
        }
    }

    return true;
}

bool LuaBridgeScriptRuntime::hasPluginTable() const {
    return m_pluginTable.valid();
}

bool LuaBridgeScriptRuntime::hasPluginFunction(const char* name) const {
    sol::protected_function function = m_pluginTable[name];
    return function.valid();
}

sol::protected_function LuaBridgeScriptRuntime::pluginFunction(const char* name) const {
    return m_pluginTable[name];
}

const sol::table& LuaBridgeScriptRuntime::pluginTable() const {
    return m_pluginTable;
}
