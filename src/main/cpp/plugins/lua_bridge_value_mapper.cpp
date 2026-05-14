#include "plugins/lua_bridge_value_mapper.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace {

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

    const std::string action = LuaBridgeValueMapper::toLowerCopy(object.as<std::string>());
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
    if (action == "list") {
        return ActionType::LIST;
    }
    if (action == "info") {
        return ActionType::INFO;
    }

    return ActionType::UNKNOWN;
}

bool packages_match_exact_without_action(const Package& left, const Package& right) {
    return left.system == right.system &&
           left.name == right.name &&
           left.version == right.version &&
           left.sourcePath == right.sourcePath &&
           left.localTarget == right.localTarget;
}

bool packages_match_loose_without_action(const Package& candidate, const Package& original) {
    if (!candidate.system.empty() && candidate.system != original.system) {
        return false;
    }
    if (!candidate.name.empty() && candidate.name != original.name) {
        return false;
    }
    if (!candidate.version.empty() && candidate.version != original.version) {
        return false;
    }
    if (!candidate.sourcePath.empty() && candidate.sourcePath != original.sourcePath) {
        return false;
    }
    if (candidate.localTarget && !original.localTarget) {
        return false;
    }
    return !candidate.name.empty() || !candidate.sourcePath.empty();
}

std::vector<std::string> string_array_from_lua_table(const sol::table& table) {
    std::vector<std::string> values;
    for (const auto& [_, value] : table) {
        if (value.is<std::string>()) {
            values.push_back(value.as<std::string>());
        }
    }
    return values;
}

std::vector<std::pair<std::string, std::string>> extra_fields_from_lua_table(const sol::table& table) {
    std::vector<std::pair<std::string, std::string>> fields;
    for (const auto& [key, value] : table) {
        if (key.is<std::string>() && !value.is<sol::table>()) {
            fields.emplace_back(key.as<std::string>(), LuaBridgeValueMapper::valueToString(value));
            continue;
        }
        if (!value.is<sol::table>()) {
            continue;
        }
        const sol::table field = value.as<sol::table>();
        const std::string fieldKey = field.get_or("key", std::string{});
        const std::string fieldValue = field.get_or("value", std::string{});
        if (!fieldKey.empty() && !fieldValue.empty()) {
            fields.emplace_back(fieldKey, fieldValue);
        }
    }
    return fields;
}

PackageInfo package_info_from_lua_table(const sol::table& info) {
    PackageInfo packageInfo;
    packageInfo.system = info.get_or("system", std::string{});
    packageInfo.name = info.get_or("name", std::string{});
    packageInfo.packageId = info.get_or("packageId", std::string{});
    packageInfo.version = info.get_or("version", std::string{});
    packageInfo.latestVersion = info.get_or("latestVersion", std::string{});
    packageInfo.status = info.get_or("status", std::string{});
    packageInfo.installed = info.get_or("installed", std::string{});
    packageInfo.summary = info.get_or("summary", std::string{});
    packageInfo.description = info.get_or("description", std::string{});
    packageInfo.homepage = info.get_or("homepage", std::string{});
    packageInfo.documentation = info.get_or("documentation", std::string{});
    packageInfo.sourceUrl = info.get_or("sourceUrl", std::string{});
    packageInfo.repository = info.get_or("repository", std::string{});
    packageInfo.channel = info.get_or("channel", std::string{});
    packageInfo.section = info.get_or("section", std::string{});
    packageInfo.packageType = info.get_or("packageType", info.get_or("type", std::string{}));
    packageInfo.architecture = info.get_or("architecture", std::string{});
    packageInfo.targetSystems = info.get_or("targetSystems", std::string{});
    packageInfo.license = info.get_or("license", std::string{});
    packageInfo.author = info.get_or("author", std::string{});
    packageInfo.maintainer = info.get_or("maintainer", std::string{});
    packageInfo.email = info.get_or("email", std::string{});
    packageInfo.publishedAt = info.get_or("publishedAt", std::string{});
    packageInfo.updatedAt = info.get_or("updatedAt", std::string{});
    packageInfo.size = info.get_or("size", std::string{});
    packageInfo.installedSize = info.get_or("installedSize", std::string{});
    if (const sol::object dependencies = info["dependencies"]; dependencies.is<sol::table>()) {
        packageInfo.dependencies = string_array_from_lua_table(dependencies.as<sol::table>());
    }
    if (const sol::object optionalDependencies = info["optionalDependencies"]; optionalDependencies.is<sol::table>()) {
        packageInfo.optionalDependencies = string_array_from_lua_table(optionalDependencies.as<sol::table>());
    }
    if (const sol::object provides = info["provides"]; provides.is<sol::table>()) {
        packageInfo.provides = string_array_from_lua_table(provides.as<sol::table>());
    }
    if (const sol::object conflicts = info["conflicts"]; conflicts.is<sol::table>()) {
        packageInfo.conflicts = string_array_from_lua_table(conflicts.as<sol::table>());
    }
    if (const sol::object replaces = info["replaces"]; replaces.is<sol::table>()) {
        packageInfo.replaces = string_array_from_lua_table(replaces.as<sol::table>());
    }
    if (const sol::object binaries = info["binaries"]; binaries.is<sol::table>()) {
        packageInfo.binaries = string_array_from_lua_table(binaries.as<sol::table>());
    }
    if (const sol::object tags = info["tags"]; tags.is<sol::table>()) {
        packageInfo.tags = string_array_from_lua_table(tags.as<sol::table>());
    }
    if (const sol::object extraFields = info["extraFields"]; extraFields.is<sol::table>()) {
        packageInfo.extraFields = extra_fields_from_lua_table(extraFields.as<sol::table>());
    }
    if (packageInfo.summary.empty()) {
        packageInfo.summary = packageInfo.description;
    }
    return packageInfo;
}

std::optional<std::vector<PluginWriteScope>> write_scopes_from_lua_object(const sol::object& value) {
    if (!value.valid() || value.is<sol::lua_nil_t>()) {
        return std::nullopt;
    }
    if (value.get_type() != sol::type::table) {
        return std::nullopt;
    }

    std::vector<PluginWriteScope> scopes;
    for (const auto& [_, entry] : value.as<sol::table>()) {
        if (entry.get_type() != sol::type::table) {
            return std::nullopt;
        }

        const sol::table scopeTable = entry.as<sol::table>();
        const sol::optional<std::string> kind = scopeTable["kind"];
        if (!kind.has_value()) {
            return std::nullopt;
        }

        PluginWriteScope scope;
        scope.kind = LuaBridgeValueMapper::toLowerCopy(kind.value());
        if (const sol::optional<std::string> rawValue = scopeTable["value"]; rawValue.has_value()) {
            scope.value = rawValue.value();
        }
        scopes.push_back(std::move(scope));
    }

    return scopes;
}

std::optional<std::vector<PluginNetworkScope>> network_scopes_from_lua_object(const sol::object& value) {
    if (!value.valid() || value.is<sol::lua_nil_t>()) {
        return std::nullopt;
    }
    if (value.get_type() != sol::type::table) {
        return std::nullopt;
    }

    std::vector<PluginNetworkScope> scopes;
    for (const auto& [_, entry] : value.as<sol::table>()) {
        if (entry.get_type() != sol::type::table) {
            return std::nullopt;
        }

        const sol::table scopeTable = entry.as<sol::table>();
        PluginNetworkScope scope;
        if (const sol::optional<std::string> host = scopeTable["host"]; host.has_value()) {
            scope.host = LuaBridgeValueMapper::toLowerCopy(host.value());
        }
        if (const sol::optional<std::string> scheme = scopeTable["scheme"]; scheme.has_value()) {
            scope.scheme = LuaBridgeValueMapper::toLowerCopy(scheme.value());
        }
        if (const sol::optional<std::string> pathPrefix = scopeTable["pathPrefix"]; pathPrefix.has_value()) {
            scope.pathPrefix = pathPrefix.value();
        }
        if (scope.host.empty() && scope.scheme.empty() && scope.pathPrefix.empty()) {
            return std::nullopt;
        }
        scopes.push_back(std::move(scope));
    }

    return scopes;
}

}  // namespace

std::string LuaBridgeValueMapper::toLowerCopy(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

std::string LuaBridgeValueMapper::valueToString(const sol::object& value) {
    if (!value.valid()) {
        return "null";
    }
    if (value.is<std::string>()) {
        return value.as<std::string>();
    }
    if (value.is<bool>()) {
        return value.as<bool>() ? "true" : "false";
    }
    if (value.is<int>()) {
        return std::to_string(value.as<int>());
    }
    if (value.is<double>()) {
        std::ostringstream stream;
        stream << value.as<double>();
        return stream.str();
    }
    return "<lua-value>";
}

std::string LuaBridgeValueMapper::serializeLuaPayload(const sol::object& value) {
    if (!value.valid()) {
        return "null";
    }
    if (value.is<std::string>() || value.is<bool>() || value.is<int>() || value.is<double>()) {
        return valueToString(value);
    }
    if (value.get_type() != sol::type::table) {
        return "<lua-value>";
    }

    std::ostringstream stream;
    stream << '{';
    std::vector<std::pair<std::string, std::string>> fields;
    for (const auto& [key, entry] : value.as<sol::table>()) {
        fields.emplace_back(valueToString(key), valueToString(entry));
    }
    std::sort(fields.begin(), fields.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    bool first = true;
    for (const auto& [keyText, valueText] : fields) {
        if (!first) {
            stream << ", ";
        }
        first = false;
        stream << keyText << '=' << valueText;
    }
    stream << '}';
    return stream.str();
}

std::optional<std::vector<std::string>> LuaBridgeValueMapper::stringArrayFromObject(const sol::object& value) {
    if (!value.valid() || value.is<sol::lua_nil_t>()) {
        return std::nullopt;
    }
    if (value.get_type() == sol::type::userdata && value.is<std::vector<std::string>>()) {
        return value.as<std::vector<std::string>>();
    }
    if (value.get_type() != sol::type::table) {
        return std::nullopt;
    }

    std::vector<std::string> result;
    for (const auto& [_, entry] : value.as<sol::table>()) {
        if (entry.get_type() != sol::type::string) {
            return std::nullopt;
        }
        result.push_back(entry.as<std::string>());
    }
    return result;
}

std::optional<std::vector<Package>> LuaBridgeValueMapper::packagesFromObject(const sol::object& value) {
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
        if (const sol::optional<std::string> sourcePath = packageTable["sourcePath"]; sourcePath.has_value()) {
            package.sourcePath = sourcePath.value();
        }
        if (const sol::optional<bool> localTarget = packageTable["localTarget"]; localTarget.has_value()) {
            package.localTarget = localTarget.value();
        }
        if (const sol::optional<std::vector<std::string>> flags = packageTable["flags"]; flags.has_value()) {
            package.flags = flags.value();
        }

        packages.push_back(std::move(package));
    }

    return packages;
}

std::optional<Package> LuaBridgeValueMapper::packageFromObject(const sol::object& value) {
    if (!value.valid()) {
        return std::nullopt;
    }
    if (value.get_type() == sol::type::userdata) {
        return value.as<Package>();
    }
    if (value.get_type() != sol::type::table) {
        return std::nullopt;
    }

    const sol::table packageTable = value.as<sol::table>();
    Package package;
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
    if (const sol::optional<std::string> sourcePath = packageTable["sourcePath"]; sourcePath.has_value()) {
        package.sourcePath = sourcePath.value();
    }
    if (const sol::optional<bool> localTarget = packageTable["localTarget"]; localTarget.has_value()) {
        package.localTarget = localTarget.value();
    }
    if (const sol::optional<std::vector<std::string>> flags = packageTable["flags"]; flags.has_value()) {
        package.flags = flags.value();
    }
    return package;
}

void LuaBridgeValueMapper::inheritMissingPackageFields(const std::vector<Package>& sourcePackages, std::vector<Package>& packages) {
    std::vector<bool> matched(sourcePackages.size(), false);

    auto apply_defaults = [](Package& target, const Package& source) {
        if (target.action == ActionType::UNKNOWN) {
            target.action = source.action;
        }
        if (target.system.empty()) {
            target.system = source.system;
        }
        if (target.name.empty()) {
            target.name = source.name;
        }
        if (target.version.empty()) {
            target.version = source.version;
        }
        if (target.sourcePath.empty()) {
            target.sourcePath = source.sourcePath;
        }
        if (target.flags.empty()) {
            target.flags = source.flags;
        }
        target.localTarget = target.localTarget || source.localTarget;
        target.directRequest = target.directRequest || source.directRequest;
    };

    auto find_match_index = [&](const Package& candidate, const bool loose) {
        for (std::size_t index = 0; index < sourcePackages.size(); ++index) {
            if (matched[index]) {
                continue;
            }
            const bool matches = loose
                ? packages_match_loose_without_action(candidate, sourcePackages[index])
                : packages_match_exact_without_action(candidate, sourcePackages[index]);
            if (matches) {
                return index;
            }
        }
        return sourcePackages.size();
    };

    for (Package& package : packages) {
        std::size_t matchIndex = find_match_index(package, false);
        if (matchIndex == sourcePackages.size()) {
            matchIndex = find_match_index(package, true);
        }
        if (matchIndex == sourcePackages.size()) {
            continue;
        }
        matched[matchIndex] = true;
        apply_defaults(package, sourcePackages[matchIndex]);
    }
}

std::vector<PackageInfo> LuaBridgeValueMapper::packageInfoListFromObject(const sol::object& value) {
    if (!value.valid() || value.get_type() != sol::type::table) {
        return {};
    }

    std::vector<PackageInfo> result;
    const sol::table table = value.as<sol::table>();
    for (const auto& [_, entry] : table) {
        if (entry.get_type() == sol::type::userdata) {
            result.push_back(entry.as<PackageInfo>());
            continue;
        }
        if (entry.get_type() != sol::type::table) {
            continue;
        }
        result.push_back(package_info_from_lua_table(entry.as<sol::table>()));
    }
    return result;
}

PackageInfo LuaBridgeValueMapper::packageInfoFromObject(const sol::object& value) {
    if (!value.valid()) {
        return {};
    }
    if (value.get_type() == sol::type::userdata) {
        return value.as<PackageInfo>();
    }
    if (value.get_type() != sol::type::table) {
        return {};
    }
    return package_info_from_lua_table(value.as<sol::table>());
}

std::optional<ProxyResolution> LuaBridgeValueMapper::proxyResolutionFromObject(const sol::object& value) {
    if (!value.valid() || value.is<sol::lua_nil_t>() || value.get_type() != sol::type::table) {
        return std::nullopt;
    }

    const sol::table table = value.as<sol::table>();
    const sol::optional<std::string> targetSystem = table["targetSystem"];
    if (!targetSystem.has_value()) {
        return std::nullopt;
    }

    ProxyResolution resolution;
    resolution.targetSystem = targetSystem.value();

    if (const sol::object packagesObject = table["packages"]; packagesObject.valid() && !packagesObject.is<sol::lua_nil_t>()) {
        const auto packages = stringArrayFromObject(packagesObject);
        if (!packages.has_value()) {
            return std::nullopt;
        }
        resolution.packages = packages.value();
    }

    if (const sol::optional<std::string> localPath = table["localPath"]; localPath.has_value()) {
        resolution.localPath = localPath.value();
    }

    if (const sol::object flagsObject = table["flags"]; flagsObject.valid() && !flagsObject.is<sol::lua_nil_t>()) {
        const auto flags = stringArrayFromObject(flagsObject);
        if (!flags.has_value()) {
            return std::nullopt;
        }
        resolution.flags = flags.value();
    }

    return resolution;
}

std::optional<PluginSecurityMetadata> LuaBridgeValueMapper::pluginSecurityMetadataFromObject(const sol::object& value) {
    if (!value.valid() || value.get_type() != sol::type::table) {
        return std::nullopt;
    }

    const sol::table metadata = value.as<sol::table>();
    PluginSecurityMetadata parsed;
    bool hasAnyField = false;

    if (const sol::optional<std::string> role = metadata["role"]; role.has_value()) {
        parsed.role = toLowerCopy(role.value());
        hasAnyField = true;
    }
    if (const auto capabilities = stringArrayFromObject(metadata["capabilities"]); capabilities.has_value()) {
        parsed.capabilities.reserve(capabilities->size());
        for (const std::string& capability : capabilities.value()) {
            parsed.capabilities.push_back(toLowerCopy(capability));
        }
        hasAnyField = true;
    }
    if (const auto ecosystemScopes = stringArrayFromObject(metadata["ecosystemScopes"]); ecosystemScopes.has_value()) {
        parsed.ecosystemScopes = ecosystemScopes.value();
        hasAnyField = true;
    }
    if (const auto writeScopes = write_scopes_from_lua_object(metadata["writeScopes"]); writeScopes.has_value()) {
        parsed.writeScopes = writeScopes.value();
        hasAnyField = true;
    }
    if (const auto networkScopes = network_scopes_from_lua_object(metadata["networkScopes"]); networkScopes.has_value()) {
        parsed.networkScopes = networkScopes.value();
        hasAnyField = true;
    }
    if (const sol::optional<std::string> privilegeLevel = metadata["privilegeLevel"]; privilegeLevel.has_value()) {
        parsed.privilegeLevel = toLowerCopy(privilegeLevel.value());
        hasAnyField = true;
    }
    if (const sol::optional<std::string> osvEcosystem = metadata["osvEcosystem"]; osvEcosystem.has_value()) {
        parsed.osvEcosystem = osvEcosystem.value();
        hasAnyField = true;
    }
    if (const sol::optional<std::string> purlType = metadata["purlType"]; purlType.has_value()) {
        parsed.purlType = purlType.value();
        hasAnyField = true;
    }
    if (const sol::optional<std::string> comparatorProfile = metadata["versionComparatorProfile"]; comparatorProfile.has_value()) {
        parsed.versionComparator.profile = comparatorProfile.value();
        hasAnyField = true;
    }
    if (const sol::optional<std::string> tokenPattern = metadata["versionTokenPattern"]; tokenPattern.has_value()) {
        parsed.versionComparator.tokenPattern = tokenPattern.value();
        hasAnyField = true;
    }
    if (const sol::optional<bool> caseInsensitive = metadata["versionCaseInsensitive"]; caseInsensitive.has_value()) {
        parsed.versionComparator.caseInsensitive = caseInsensitive.value();
        hasAnyField = true;
    }

    return hasAnyField ? std::optional<PluginSecurityMetadata>(parsed) : std::nullopt;
}

std::vector<std::string> LuaBridgeValueMapper::fileExtensionsFromPluginTable(const sol::table& pluginTable) {
    std::vector<std::string> fileExtensions;
    const sol::object fileExtObj = pluginTable["fileExtensions"];
    if (fileExtObj.valid() && fileExtObj.get_type() == sol::type::table) {
        const sol::table extTable = fileExtObj.as<sol::table>();
        extTable.for_each([&fileExtensions](const sol::object&, const sol::object& value) {
            if (value.valid() && value.get_type() == sol::type::string) {
                fileExtensions.push_back(value.as<std::string>());
            }
        });
    }
    return fileExtensions;
}
