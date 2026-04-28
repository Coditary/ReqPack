#include "core/manifest_loader.h"

#include <sol/sol.hpp>

#include <stdexcept>
#include <string>

std::vector<ManifestEntry> ManifestLoader::load(const std::filesystem::path& manifestPath) {
    if (!std::filesystem::exists(manifestPath)) {
        throw std::runtime_error("manifest not found: " + manifestPath.string());
    }

    sol::state lua;
    lua.open_libraries(
        sol::lib::base,
        sol::lib::package,
        sol::lib::table,
        sol::lib::string,
        sol::lib::math
    );

    sol::load_result loadResult = lua.load_file(manifestPath.string());
    if (!loadResult.valid()) {
        const sol::error err = loadResult;
        throw std::runtime_error("failed to parse manifest: " + std::string(err.what()));
    }

    const sol::protected_function_result execResult = loadResult();
    if (!execResult.valid()) {
        const sol::error err = execResult;
        throw std::runtime_error("failed to execute manifest: " + std::string(err.what()));
    }

    // Resolve the packages table.
    // Priority: returned table { packages = {...} } > global variable `packages`
    sol::optional<sol::table> packagesTable;

    if (execResult.get_type() == sol::type::table) {
        const sol::table root = execResult;
        // Guard: do not assign nil to sol::optional<sol::table> — sol2 panics.
        const sol::object packagesObj = root["packages"];
        if (packagesObj.get_type() == sol::type::table) {
            packagesTable = packagesObj.as<sol::table>();
        }
    }

    if (!packagesTable.has_value()) {
        const sol::object globalPackages = lua["packages"];
        if (globalPackages.get_type() == sol::type::table) {
            packagesTable = globalPackages.as<sol::table>();
        }
    }

    if (!packagesTable.has_value()) {
        throw std::runtime_error(
            "manifest '" + manifestPath.string() + "' has no 'packages' table"
        );
    }

    std::vector<ManifestEntry> entries;
    std::size_t entryIndex = 0;

    for (const auto& [key, value] : packagesTable.value()) {
        ++entryIndex;

        if (value.get_type() != sol::type::table) {
            throw std::runtime_error(
                "manifest entry #" + std::to_string(entryIndex) + " is not a table"
            );
        }

        const sol::table pkg = value.as<sol::table>();

        const sol::optional<std::string> system = pkg["system"];
        if (!system.has_value() || system.value().empty()) {
            throw std::runtime_error(
                "manifest entry #" + std::to_string(entryIndex) + " is missing 'system'"
            );
        }

        const sol::optional<std::string> name = pkg["name"];
        if (!name.has_value() || name.value().empty()) {
            throw std::runtime_error(
                "manifest entry #" + std::to_string(entryIndex) + " is missing 'name'"
            );
        }

        ManifestEntry entry;
        entry.system = system.value();
        entry.name   = name.value();

        const sol::optional<std::string> version = pkg["version"];
        if (version.has_value() && !version.value().empty()) {
            entry.version = version.value();
        }

        const sol::optional<sol::table> flags = pkg["flags"];
        if (flags.has_value()) {
            for (const auto& [_, flagVal] : flags.value()) {
                if (flagVal.get_type() == sol::type::string) {
                    entry.flags.push_back(flagVal.as<std::string>());
                }
            }
        }

        entries.push_back(std::move(entry));
    }

    return entries;
}
