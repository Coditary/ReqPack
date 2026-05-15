#include "configuration_internal.h"

#include <set>

namespace {

std::optional<RepositoryAuthType> repository_auth_type_from_string(const std::string& value) {
    const std::string normalized = configuration_internal::to_lower_copy(value);
    if (normalized == "none") {
        return RepositoryAuthType::NONE;
    }
    if (normalized == "basic") {
        return RepositoryAuthType::BASIC;
    }
    if (normalized == "token") {
        return RepositoryAuthType::TOKEN;
    }
    if (normalized == "ssh") {
        return RepositoryAuthType::SSH;
    }

    return std::nullopt;
}

std::optional<RepositoryChecksumPolicy> repository_checksum_policy_from_string(const std::string& value) {
    const std::string normalized = configuration_internal::to_lower_copy(value);
    if (normalized == "fail") {
        return RepositoryChecksumPolicy::FAIL;
    }
    if (normalized == "warn" || normalized == "warning") {
        return RepositoryChecksumPolicy::WARN;
    }
    if (normalized == "ignore") {
        return RepositoryChecksumPolicy::IGNORE;
    }

    return std::nullopt;
}

std::optional<RepositoryAuthConfig> load_repository_auth_config(const sol::object& object) {
    RepositoryAuthConfig auth;
    if (!object.valid()) {
        return auth;
    }
    if (object.get_type() != sol::type::table) {
        return std::nullopt;
    }

    const sol::table authTable = object.as<sol::table>();
    if (const sol::optional<std::string> typeValue = authTable["type"]; typeValue.has_value()) {
        const auto convertedType = repository_auth_type_from_string(typeValue.value());
        if (!convertedType.has_value()) {
            return std::nullopt;
        }
        auth.type = convertedType.value();
    }

    if (const sol::optional<std::string> username = authTable["username"]; username.has_value()) {
        auth.username = configuration_internal::expand_env_reference(username.value());
    }
    if (const sol::optional<std::string> password = authTable["password"]; password.has_value()) {
        auth.password = configuration_internal::expand_env_reference(password.value());
    }
    if (const sol::optional<std::string> token = authTable["token"]; token.has_value()) {
        auth.token = configuration_internal::expand_env_reference(token.value());
    }
    if (const sol::optional<std::string> sshKey = authTable["sshKey"]; sshKey.has_value()) {
        auth.sshKey = configuration_internal::expand_user_path(configuration_internal::expand_env_reference(sshKey.value())).string();
    }
    if (const sol::optional<std::string> headerName = authTable["headerName"]; headerName.has_value()) {
        auth.headerName = configuration_internal::expand_env_reference(headerName.value());
    }

    switch (auth.type) {
        case RepositoryAuthType::NONE:
            if (!auth.username.empty() || !auth.password.empty() || !auth.token.empty() || !auth.sshKey.empty() || !auth.headerName.empty()) {
                return std::nullopt;
            }
            return auth;
        case RepositoryAuthType::BASIC:
            return (!auth.username.empty() && !auth.password.empty()) ? std::optional<RepositoryAuthConfig>(auth) : std::nullopt;
        case RepositoryAuthType::TOKEN:
            return !auth.token.empty() ? std::optional<RepositoryAuthConfig>(auth) : std::nullopt;
        case RepositoryAuthType::SSH:
            return !auth.sshKey.empty() ? std::optional<RepositoryAuthConfig>(auth) : std::nullopt;
        default:
            return std::nullopt;
    }
}

RepositoryValidationConfig load_repository_validation_config(const sol::object& object) {
    RepositoryValidationConfig validation;
    if (!object.valid() || object.get_type() != sol::type::table) {
        return validation;
    }

    const sol::table validationTable = object.as<sol::table>();
    if (const sol::optional<std::string> checksum = validationTable["checksum"]; checksum.has_value()) {
        if (const auto converted = repository_checksum_policy_from_string(checksum.value())) {
            validation.checksum = converted.value();
        }
    }
    configuration_internal::assign_if_present(validationTable, "tlsVerify", validation.tlsVerify);
    return validation;
}

RepositoryScopeConfig load_repository_scope_config(const sol::object& object) {
    RepositoryScopeConfig scope;
    if (!object.valid() || object.get_type() != sol::type::table) {
        return scope;
    }

    const sol::table scopeTable = object.as<sol::table>();
    if (!configuration_internal::load_string_array_strict(scopeTable["include"], scope.include)) {
        scope.include.clear();
    }
    if (!configuration_internal::load_string_array_strict(scopeTable["exclude"], scope.exclude)) {
        scope.exclude.clear();
    }
    return scope;
}

std::optional<RepositoryExtraValue> load_repository_extra_value(const sol::object& object) {
    if (!object.valid()) {
        return std::nullopt;
    }
    if (object.get_type() == sol::type::string) {
        return RepositoryExtraValue{object.as<std::string>()};
    }
    if (object.get_type() == sol::type::boolean) {
        return RepositoryExtraValue{object.as<bool>()};
    }
    if (object.get_type() == sol::type::number) {
        return RepositoryExtraValue{object.as<double>()};
    }
    if (object.get_type() != sol::type::table) {
        return std::nullopt;
    }

    std::vector<std::string> values;
    if (!configuration_internal::load_string_array_strict(object, values)) {
        return std::nullopt;
    }
    return RepositoryExtraValue{std::move(values)};
}

std::optional<RepositoryEntry> load_repository_entry(const sol::table& table) {
    RepositoryEntry entry;
    configuration_internal::assign_if_present(table, "id", entry.id);
    configuration_internal::assign_if_present(table, "url", entry.url);
    if (entry.id.empty() || entry.url.empty()) {
        return std::nullopt;
    }

    configuration_internal::assign_if_present(table, "priority", entry.priority);
    configuration_internal::assign_if_present(table, "enabled", entry.enabled);
    configuration_internal::assign_if_present(table, "type", entry.type);

    const auto auth = load_repository_auth_config(table["auth"]);
    if (!auth.has_value()) {
        return std::nullopt;
    }
    entry.auth = auth.value();
    entry.validation = load_repository_validation_config(table["validation"]);
    entry.scope = load_repository_scope_config(table["scope"]);

    for (const auto& [key, value] : table) {
        if (key.get_type() != sol::type::string) {
            continue;
        }

        const std::string field = key.as<std::string>();
        if (field == "id" || field == "url" || field == "priority" || field == "enabled" || field == "type" ||
            field == "auth" || field == "validation" || field == "scope") {
            continue;
        }

        if (const auto extra = load_repository_extra_value(value)) {
            entry.extras[field] = extra.value();
        }
    }

    return entry;
}

}  // namespace

namespace configuration_internal {

std::map<std::string, std::vector<std::string>> load_string_list_map(const sol::object& object, bool& ok) {
    std::map<std::string, std::vector<std::string>> values;
    ok = true;
    if (!object.valid()) {
        return values;
    }
    if (object.get_type() != sol::type::table) {
        ok = false;
        return values;
    }

    for (const auto& [key, value] : object.as<sol::table>()) {
        if (key.get_type() != sol::type::string) {
            ok = false;
            values.clear();
            return values;
        }

        std::vector<std::string> members;
        if (!load_string_array_strict(value, members)) {
            ok = false;
            values.clear();
            return values;
        }

        members = normalize_string_list(std::move(members));
        if (members.empty()) {
            continue;
        }
        values[to_lower_copy(key.as<std::string>())] = std::move(members);
    }

    return values;
}

bool load_string_array_strict(const sol::object& object, std::vector<std::string>& values) {
    values.clear();
    if (!object.valid()) {
        return true;
    }
    if (object.get_type() != sol::type::table) {
        return false;
    }

    for (const auto& [key, value] : object.as<sol::table>()) {
        if (key.get_type() != sol::type::number || value.get_type() != sol::type::string) {
            values.clear();
            return false;
        }
        values.push_back(value.as<std::string>());
    }

    return true;
}

std::vector<std::string> load_string_array(const sol::object& object) {
    std::vector<std::string> values;
    if (!object.valid() || object.get_type() != sol::type::table) {
        return values;
    }

    for (const auto& [_, value] : object.as<sol::table>()) {
        if (value.get_type() == sol::type::string) {
            values.push_back(value.as<std::string>());
        }
    }

    return values;
}

std::vector<RegistryWriteScope> load_registry_write_scopes(const sol::object& object) {
    std::vector<RegistryWriteScope> values;
    if (!object.valid() || object.get_type() != sol::type::table) {
        return values;
    }

    for (const auto& [_, value] : object.as<sol::table>()) {
        if (value.get_type() != sol::type::table) {
            continue;
        }

        const sol::table scopeTable = value.as<sol::table>();
        const sol::optional<std::string> kind = scopeTable["kind"];
        if (!kind.has_value()) {
            continue;
        }

        RegistryWriteScope scope;
        scope.kind = to_lower_copy(kind.value());
        if (const sol::optional<std::string> rawValue = scopeTable["value"]; rawValue.has_value()) {
            scope.value = rawValue.value();
        }
        values.push_back(std::move(scope));
    }

    return values;
}

std::vector<RegistryNetworkScope> load_registry_network_scopes(const sol::object& object) {
    std::vector<RegistryNetworkScope> values;
    if (!object.valid() || object.get_type() != sol::type::table) {
        return values;
    }

    for (const auto& [_, value] : object.as<sol::table>()) {
        if (value.get_type() != sol::type::table) {
            continue;
        }

        const sol::table scopeTable = value.as<sol::table>();
        RegistryNetworkScope scope;
        if (const sol::optional<std::string> host = scopeTable["host"]; host.has_value()) {
            scope.host = to_lower_copy(host.value());
        }
        if (const sol::optional<std::string> scheme = scopeTable["scheme"]; scheme.has_value()) {
            scope.scheme = to_lower_copy(scheme.value());
        }
        if (const sol::optional<std::string> pathPrefix = scopeTable["pathPrefix"]; pathPrefix.has_value()) {
            scope.pathPrefix = pathPrefix.value();
        }
        if (scope.host.empty() && scope.scheme.empty() && scope.pathPrefix.empty()) {
            continue;
        }
        values.push_back(std::move(scope));
    }

    return values;
}

RegistrySourceMap load_registry_sources_from_table(const sol::table& table) {
    RegistrySourceMap sources;

    for (const auto& [key, value] : table) {
        if (key.get_type() != sol::type::string) {
            continue;
        }

        RegistrySourceEntry entry;
        if (value.get_type() == sol::type::string) {
            entry.source = value.as<std::string>();
        } else if (value.get_type() == sol::type::table) {
            const sol::table entryTable = value.as<sol::table>();

            if (const sol::optional<std::string> source = entryTable["source"]; source.has_value()) {
                entry.source = source.value();
            } else if (const sol::optional<std::string> url = entryTable["url"]; url.has_value()) {
                entry.source = url.value();
            } else if (const sol::optional<std::string> path = entryTable["path"]; path.has_value()) {
                entry.source = path.value();
            } else if (const sol::optional<std::string> target = entryTable["target"]; target.has_value()) {
                entry.source = target.value();
            }

            if (const sol::optional<bool> alias = entryTable["alias"]; alias.has_value()) {
                entry.alias = alias.value();
            }
            if (const sol::optional<std::string> description = entryTable["description"]; description.has_value()) {
                entry.description = description.value();
            }
            if (const sol::optional<std::string> role = entryTable["role"]; role.has_value()) {
                entry.role = to_lower_copy(role.value());
            }
            if (const sol::optional<std::string> targetSystem = entryTable["targetSystem"]; targetSystem.has_value()) {
                entry.targetSystem = to_lower_copy(targetSystem.value());
            }
            const sol::object capabilitiesObject = entryTable["capabilities"];
            if (capabilitiesObject.valid() && capabilitiesObject.get_type() == sol::type::table) {
                for (const auto& [_, capability] : capabilitiesObject.as<sol::table>()) {
                    if (capability.get_type() == sol::type::string) {
                        const std::string normalized = to_lower_copy(capability.as<std::string>());
                        if (!normalized.empty()) {
                            entry.capabilities.push_back(normalized);
                        }
                    }
                }
            }
            entry.ecosystemScopes = load_string_array(entryTable["ecosystemScopes"]);
            for (std::string& ecosystem : entry.ecosystemScopes) {
                ecosystem = to_lower_copy(ecosystem);
            }
            entry.writeScopes = load_registry_write_scopes(entryTable["writeScopes"]);
            entry.networkScopes = load_registry_network_scopes(entryTable["networkScopes"]);
            if (const sol::optional<std::string> privilegeLevel = entryTable["privilegeLevel"]; privilegeLevel.has_value()) {
                entry.privilegeLevel = to_lower_copy(privilegeLevel.value());
            }
            if (const sol::optional<std::string> scriptSha256 = entryTable["scriptSha256"]; scriptSha256.has_value()) {
                entry.scriptSha256 = to_lower_copy(scriptSha256.value());
            }
            if (const sol::optional<std::string> bootstrapSha256 = entryTable["bootstrapSha256"]; bootstrapSha256.has_value()) {
                entry.bootstrapSha256 = to_lower_copy(bootstrapSha256.value());
            }
        } else {
            continue;
        }

        if (entry.source.empty()) {
            continue;
        }

        if (entry.alias) {
            entry.source = to_lower_copy(entry.source);
        } else {
            entry.source = expand_user_path(entry.source).string();
        }

        sources[to_lower_copy(key.as<std::string>())] = std::move(entry);
    }

    return sources;
}

std::map<std::string, std::string> load_string_map(const sol::object& object) {
    std::map<std::string, std::string> values;
    if (!object.valid() || object.get_type() != sol::type::table) {
        return values;
    }

    for (const auto& [key, value] : object.as<sol::table>()) {
        if (key.get_type() != sol::type::string || value.get_type() != sol::type::string) {
            continue;
        }

        values[to_lower_copy(key.as<std::string>())] = value.as<std::string>();
    }

    return values;
}

std::map<std::string, ProxyConfig> load_proxy_config_map(const sol::object& object) {
    std::map<std::string, ProxyConfig> values;
    if (!object.valid() || object.get_type() != sol::type::table) {
        return values;
    }

    for (const auto& [key, value] : object.as<sol::table>()) {
        if (key.get_type() != sol::type::string || value.get_type() != sol::type::table) {
            continue;
        }

        ProxyConfig proxy;
        const sol::table proxyTable = value.as<sol::table>();
        if (const sol::optional<std::string> defaultTarget = proxyTable["default"]; defaultTarget.has_value()) {
            proxy.defaultTarget = to_lower_copy(defaultTarget.value());
        }

        const std::vector<std::string> targets = load_string_array(proxyTable["targets"]);
        for (const std::string& target : targets) {
            if (!target.empty()) {
                proxy.targets.push_back(to_lower_copy(target));
            }
        }

        proxy.options = load_string_map(proxyTable["options"]);
        values[to_lower_copy(key.as<std::string>())] = std::move(proxy);
    }

    return values;
}

std::map<std::string, SecurityGatewayConfig> load_security_gateway_map(const sol::object& object) {
    std::map<std::string, SecurityGatewayConfig> values;
    if (!object.valid() || object.get_type() != sol::type::table) {
        return values;
    }

    for (const auto& [key, value] : object.as<sol::table>()) {
        if (key.get_type() != sol::type::string || value.get_type() != sol::type::table) {
            continue;
        }

        SecurityGatewayConfig gateway;
        const sol::table gatewayTable = value.as<sol::table>();
        assign_if_present(gatewayTable, "enabled", gateway.enabled);
        const std::vector<std::string> backends = load_string_array(gatewayTable["backends"]);
        if (!backends.empty()) {
            gateway.backends.clear();
            for (const std::string& backend : backends) {
                gateway.backends.push_back(to_lower_copy(backend));
            }
        }

        values[to_lower_copy(key.as<std::string>())] = std::move(gateway);
    }

    return values;
}

std::map<std::string, SecurityBackendConfig> load_security_backend_map(const sol::object& object) {
    std::map<std::string, SecurityBackendConfig> values;
    if (!object.valid() || object.get_type() != sol::type::table) {
        return values;
    }

    for (const auto& [key, value] : object.as<sol::table>()) {
        if (key.get_type() != sol::type::string || value.get_type() != sol::type::table) {
            continue;
        }

        SecurityBackendConfig backend;
        const sol::table backendTable = value.as<sol::table>();
        assign_if_present(backendTable, "enabled", backend.enabled);
        assign_if_present(backendTable, "feedUrl", backend.feedUrl);
        assign_if_present(backendTable, "refreshMode", osv_refresh_mode_from_string, backend.refreshMode);
        assign_if_present(backendTable, "refreshIntervalSeconds", backend.refreshIntervalSeconds);
        assign_if_present(backendTable, "overlayPath", backend.overlayPath);

        if (!backend.feedUrl.empty()) {
            backend.feedUrl = expand_user_path(backend.feedUrl).string();
        }
        if (!backend.overlayPath.empty()) {
            backend.overlayPath = expand_user_path(backend.overlayPath).string();
        }

        values[to_lower_copy(key.as<std::string>())] = std::move(backend);
    }

    return values;
}

std::map<std::string, std::vector<RepositoryEntry>> load_repository_map(const sol::object& object) {
    std::map<std::string, std::vector<RepositoryEntry>> values;
    if (!object.valid() || object.get_type() != sol::type::table) {
        return values;
    }

    for (const auto& [key, value] : object.as<sol::table>()) {
        if (key.get_type() != sol::type::string || value.get_type() != sol::type::table) {
            continue;
        }

        const std::string ecosystem = to_lower_copy(key.as<std::string>());
        std::set<std::string> seenIds;
        std::vector<RepositoryEntry> entries;
        for (const auto& [_, item] : value.as<sol::table>()) {
            if (item.get_type() != sol::type::table) {
                continue;
            }

            const auto entry = load_repository_entry(item.as<sol::table>());
            if (!entry.has_value()) {
                continue;
            }
            if (!seenIds.insert(entry->id).second) {
                continue;
            }
            entries.push_back(entry.value());
        }

        if (!entries.empty()) {
            values[ecosystem] = std::move(entries);
        }
    }

    return values;
}

void merge_registry_sources(RegistrySourceMap& target, const RegistrySourceMap& source) {
    for (const auto& [name, entry] : source) {
        target[name] = entry;
    }
}

}  // namespace configuration_internal
