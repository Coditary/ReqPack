#pragma once

#include <optional>
#include <string>
#include <vector>

#include <sol/sol.hpp>

#include "core/common/types.h"
#include "plugins/iplugin.h"

class LuaBridgeValueMapper {
public:
    static std::string toLowerCopy(const std::string& value);
    static std::string valueToString(const sol::object& value);
    static std::string serializeLuaPayload(const sol::object& value);

    static std::optional<std::vector<std::string>> stringArrayFromObject(const sol::object& value);
    static std::optional<std::vector<Package>> packagesFromObject(const sol::object& value);
    static std::optional<Package> packageFromObject(const sol::object& value);
    static void inheritMissingPackageFields(const std::vector<Package>& sourcePackages, std::vector<Package>& packages);

    static std::vector<PackageInfo> packageInfoListFromObject(const sol::object& value);
    static PackageInfo packageInfoFromObject(const sol::object& value);

    static std::optional<ProxyResolution> proxyResolutionFromObject(const sol::object& value);
    static std::optional<PluginSecurityMetadata> pluginSecurityMetadataFromObject(const sol::object& value);
    static std::vector<std::string> fileExtensionsFromPluginTable(const sol::table& pluginTable);
};
