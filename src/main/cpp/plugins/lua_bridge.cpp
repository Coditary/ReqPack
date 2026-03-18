#include "plugins/lua_bridge.h"
#include <iostream>

LuaBridge::LuaBridge(const std::string& scriptPath) : m_scriptPath(scriptPath) {
    m_lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table, sol::lib::string, sol::lib::os);

    m_lua.new_usertype<Package>("Package",
        "name", &Package::name,
        "version", &Package::version
    );

    m_lua.new_usertype<PackageInfo>("PackageInfo",
        "name", &PackageInfo::name,
        "version", &PackageInfo::version,
        "description", &PackageInfo::description
    );
}

bool LuaBridge::init() {
    auto loadResult = m_lua.script_file(m_scriptPath, sol::script_pass_on_error);
    if (!loadResult.valid()) {
        sol::error err = loadResult;
        std::cerr << "Lua Load Error: " << err.what() << std::endl;
        return false;
    }

    m_pluginTable = m_lua["plugin"];
    if (!m_pluginTable.valid()) {
        std::cerr << "Error: Global table 'plugin' not found in " << m_scriptPath << std::endl;
        return false;
    }

    m_name = m_pluginTable.get_or("name", std::string("Unknown Lua Plugin"));
    m_version = m_pluginTable.get_or("version", std::string("0.0.0"));

    sol::protected_function luaInit = m_pluginTable["init"];
    if (luaInit.valid()) {
        auto result = luaInit();
        return result.valid() ? result.get<bool>() : false;
    }

    return true;
}

bool LuaBridge::shutdown() {
    sol::protected_function luaShutdown = m_pluginTable["shutdown"];
    if (luaShutdown.valid()) {
        auto result = luaShutdown();
        return result.valid() ? result.get<bool>() : false;
    }
    return true;
}

std::vector<std::string> LuaBridge::getCategories() {
    sol::protected_function func = m_pluginTable["getCategories"];
    if (func.valid()) {
        auto result = func();
        if (result.valid()) return result.get<std::vector<std::string>>();
    }
    return {};
}




void LuaBridge::install(const std::vector<Package>& packages) {
    sol::protected_function func = m_pluginTable["install"];
    if (func.valid()) {
        auto result = func(packages);
        if (!result.valid()) {
            sol::error err = result;
            std::cerr << "Lua Error (install): " << err.what() << std::endl;
        }
    }
}

void LuaBridge::remove(const std::vector<Package>& packages) {
    sol::protected_function func = m_pluginTable["remove"];
    if (func.valid()) {
        func(packages);
    }
}

void LuaBridge::update(const std::vector<Package>& packages) {
    sol::protected_function func = m_pluginTable["update"];
    if (func.valid()) {
        func(packages);
    }
}




std::vector<Package> LuaBridge::getRequirements() {
    sol::protected_function func = m_pluginTable["getRequirements"];
    if (func.valid()) {
        auto result = func();
        if (result.valid()) return result.get<std::vector<Package>>();
    }
    return {};
}

std::vector<PackageInfo> LuaBridge::list() {
    sol::protected_function func = m_pluginTable["list"];
    if (func.valid()) {
        auto result = func();
        if (result.valid()) return result.get<std::vector<PackageInfo>>();
    }
    return {};
}

std::vector<PackageInfo> LuaBridge::search(const std::string& prompt) {
    sol::protected_function func = m_pluginTable["search"];
    if (func.valid()) {
        auto result = func(prompt);
        if (result.valid()) return result.get<std::vector<PackageInfo>>();
    }
    return {};
}

PackageInfo LuaBridge::info(const std::string& packageName) {
    sol::protected_function func = m_pluginTable["info"];
    if (func.valid()) {
        auto result = func(packageName);
        if (result.valid()) return result.get<PackageInfo>();
    }
    return {};
}
