#include "plugins/lua_bridge.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <optional>

namespace {

std::string to_lower_copy(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

ActionType action_from_lua_object(const sol::object& object) {
    if (!object.valid()) {
        return ActionType::UNKNOWN;
    }

    if (object.is<int>()) {
        return static_cast<ActionType>(object.as<int>());
    }

    if (object.is<double>()) {
        return static_cast<ActionType>(object.as<int>());
    }

    if (!object.is<std::string>()) {
        return ActionType::UNKNOWN;
    }

    const std::string action = to_lower_copy(object.as<std::string>());
    if (action == "install") {
        return ActionType::INSTALL;
    }
    if (action == "remove") {
        return ActionType::REMOVE;
    }
    if (action == "update") {
        return ActionType::UPDATE;
    }
    if (action == "search") {
        return ActionType::SEARCH;
    }

    return ActionType::UNKNOWN;
}

std::optional<std::vector<Package>> packages_from_lua_object(const sol::object& value) {
	if (!value.valid() || value.get_type() != sol::type::table) {
		return std::nullopt;
	}

	std::vector<Package> packages;
	const sol::table packageList = value.as<sol::table>();
	for (const auto& [_, entry] : packageList) {
		Package package;

        if (entry.get_type() == sol::type::userdata) {
            package = entry.as<Package>();
            packages.push_back(std::move(package));
            continue;
        }

        if (entry.get_type() != sol::type::table) {
            continue;
        }

        const sol::table packageTable = entry.as<sol::table>();
        package.action = action_from_lua_object(packageTable["action"]);
        if (const sol::optional<std::string> system = packageTable["system"]; system.has_value()) {
            package.system = system.value();
        }
        if (const sol::optional<std::string> name = packageTable["name"]; name.has_value()) {
            package.name = name.value();
        }
        if (const sol::optional<std::string> version = packageTable["version"]; version.has_value()) {
            package.version = version.value();
        }

		packages.push_back(std::move(package));
	}

	return packages;
}

void register_types(sol::state& lua) {
    lua.new_usertype<Package>(
        "Package",
        sol::constructors<Package()>(),
        "action", sol::property(
            [](const Package& package) {
                return static_cast<int>(package.action);
            },
            [](Package& package, int action) {
                package.action = static_cast<ActionType>(action);
            }
        ),
        "system", &Package::system,
        "name", &Package::name,
        "version", &Package::version
    );

    lua.new_usertype<PackageInfo>(
        "PackageInfo",
        sol::constructors<PackageInfo()>(),
        "name", &PackageInfo::name,
        "version", &PackageInfo::version,
        "description", &PackageInfo::description,
        "homepage", &PackageInfo::homepage,
        "author", &PackageInfo::author,
        "email", &PackageInfo::email
    );
}

bool execute_file(sol::state& lua, const std::string& path) {
    sol::load_result loadResult = lua.load_file(path);
    if (!loadResult.valid()) {
        sol::error err = loadResult;
        std::cerr << "[Lua Load Error] " << path << ": " << err.what() << std::endl;
        return false;
    }

    const sol::protected_function_result executionResult = loadResult();
    if (!executionResult.valid()) {
        sol::error err = executionResult;
        std::cerr << "[Lua Exec Error] " << path << ": " << err.what() << std::endl;
        return false;
    }

    return true;
}

}  // namespace

LuaBridge::LuaBridge(const std::string& scriptPath) : m_scriptPath(scriptPath) {
    m_lua.open_libraries(sol::lib::base, sol::lib::os, sol::lib::io, sol::lib::table, sol::lib::string, sol::lib::math);
    register_types(m_lua);

    const std::filesystem::path resolvedScriptPath(scriptPath);
    m_pluginDirectory = resolvedScriptPath.parent_path().string();
    m_pluginId = resolvedScriptPath.stem().string();
    m_bootstrapPath = (resolvedScriptPath.parent_path() / "bootstrap.lua").string();

    m_lua["REQPACK_PLUGIN_ID"] = m_pluginId;
    m_lua["REQPACK_PLUGIN_DIR"] = m_pluginDirectory;
    m_lua["REQPACK_PLUGIN_SCRIPT"] = m_scriptPath;
    m_lua["REQPACK_PLUGIN_BOOTSTRAP"] = m_bootstrapPath;

    if (std::filesystem::exists(m_bootstrapPath) && !execute_file(m_lua, m_bootstrapPath)) {
        return;
    }

    if (!execute_file(m_lua, m_scriptPath)) {
        return;
    }

    m_pluginTable = m_lua["plugin"];
    if (m_pluginTable.valid()) {
        m_name = m_pluginTable.get_or("name", m_pluginId);
        m_version = m_pluginTable.get_or("version", std::string("0.0.0"));
    }
}

bool LuaBridge::init() {
    if (!m_pluginTable.valid()) {
        return false;
    }

    if (m_pluginTable["getMissingPackages"].get_type() != sol::type::function) {
        std::cerr << "[Lua API Error] getMissingPackages(packages) is required." << std::endl;
        return false;
    }

    sol::protected_function bootstrap = m_lua["bootstrap"];
    if (bootstrap.valid()) {
        auto bootstrapResult = bootstrap();
        if (!bootstrapResult.valid()) {
            sol::error err = bootstrapResult;
            std::cerr << "[Lua Exec Error] bootstrap(): " << err.what() << std::endl;
            return false;
        }

        if (bootstrapResult.return_count() > 0 && !bootstrapResult.get<bool>()) {
            return false;
        }
    }

    sol::protected_function luaInit = m_pluginTable["init"];
    if (luaInit.valid()) {
        auto result = luaInit();
        if (!result.valid()) {
            sol::error err = result;
            std::cerr << "[Lua Exec Error] init(): " << err.what() << std::endl;
            return false;
        }
        return result.return_count() == 0 ? true : result.get<bool>();
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
        if (result.valid()) {
            return result.get<std::vector<std::string>>();
        }
    }
    return {};
}

std::vector<Package> LuaBridge::getMissingPackages(const std::vector<Package>& packages) {
	sol::protected_function func = m_pluginTable["getMissingPackages"];
    if (!func.valid()) {
        std::cerr << "[Lua API Error] getMissingPackages(packages) is required." << std::endl;
        return packages;
    }

	auto result = func(packages);
    if (result.valid()) {
		if (result.return_count() == 0) {
			std::cerr << "[Lua API Error] getMissingPackages(packages) must return a package list." << std::endl;
			return packages;
		}

		if (const sol::object value = result.get<sol::object>(); value.valid()) {
			if (const auto missingPackages = packages_from_lua_object(value)) {
				return missingPackages.value();
			}
		}

		std::cerr << "[Lua API Error] getMissingPackages(packages) must return a package list." << std::endl;
		return packages;
    }

	sol::error err = result;
	std::cerr << "Lua Error (getMissingPackages): " << err.what() << std::endl;
	return packages;
}

bool LuaBridge::install(const std::vector<Package>& packages) {
    sol::protected_function func = m_pluginTable["install"];
    if (func.valid()) {
        auto result = func(packages);
        if (!result.valid()) {
            sol::error err = result;
            std::cerr << "Lua Error (install): " << err.what() << std::endl;
            return false;
        }
		return result.return_count() == 0 ? true : result.get<bool>();
    }
	return false;
}

bool LuaBridge::remove(const std::vector<Package>& packages) {
    sol::protected_function func = m_pluginTable["remove"];
    if (func.valid()) {
		auto result = func(packages);
		return result.valid() ? (result.return_count() == 0 ? true : result.get<bool>()) : false;
    }
	return false;
}

bool LuaBridge::update(const std::vector<Package>& packages) {
    sol::protected_function func = m_pluginTable["update"];
    if (func.valid()) {
		auto result = func(packages);
		return result.valid() ? (result.return_count() == 0 ? true : result.get<bool>()) : false;
    }
	return false;
}

std::vector<Package> LuaBridge::getRequirements() {
    sol::protected_function func = m_pluginTable["getRequirements"];
    if (func.valid()) {
        auto result = func();
        if (result.valid() && result.return_count() > 0) {
			if (const sol::object value = result.get<sol::object>(); value.valid()) {
				if (const auto requirements = packages_from_lua_object(value)) {
					return requirements.value();
				}
			}
        }
    }
    return {};
}

std::vector<PackageInfo> LuaBridge::list() {
    sol::protected_function func = m_pluginTable["list"];
    if (func.valid()) {
        auto result = func();
        if (result.valid()) {
            return result.get<std::vector<PackageInfo>>();
        }
    }
    return {};
}

std::vector<PackageInfo> LuaBridge::search(const std::string& prompt) {
    sol::protected_function func = m_pluginTable["search"];
    if (func.valid()) {
        auto result = func(prompt);
        if (result.valid()) {
            return result.get<std::vector<PackageInfo>>();
        }
    }
    return {};
}

PackageInfo LuaBridge::info(const std::string& packageName) {
    sol::protected_function func = m_pluginTable["info"];
    if (func.valid()) {
        auto result = func(packageName);
        if (result.valid()) {
            return result.get<PackageInfo>();
        }
    }
    return {};
}
