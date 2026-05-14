#pragma once

#include <string>

#include <sol/sol.hpp>

#include "output/logger.h"

void log_lua_error(Logger& logger, const std::string& scope, const std::string& message);

class LuaBridgeScriptRuntime {
public:
    LuaBridgeScriptRuntime();

    sol::state& state();
    const sol::state& state() const;

    bool loadScript(Logger& logger, const std::string& path);
    bool validatePluginContract(Logger& logger, const std::string& pluginId) const;

    bool hasPluginTable() const;
    bool hasPluginFunction(const char* name) const;
    sol::protected_function pluginFunction(const char* name) const;
    const sol::table& pluginTable() const;

private:
    sol::state m_lua;
    sol::table m_pluginTable;
};
