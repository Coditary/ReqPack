#include "plugins/lua_bridge.h"

#include "core/archive_resolver.h"
#include "core/downloader.h"
#include "output/progress_metrics_lua.h"
#include "plugins/exec_rules.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <type_traits>
#include <optional>
#include <sstream>

namespace {

std::string to_lower_copy(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

constexpr const char* SILENT_RUNTIME_FLAG = "__reqpack-internal-silent-runtime";

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
    if (action == "list") {
        return ActionType::LIST;
    }
    if (action == "info") {
        return ActionType::INFO;
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

std::optional<Package> package_from_lua_object(const sol::object& value) {
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

std::optional<std::vector<std::string>> string_array_from_lua_object(const sol::object& value) {
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

std::optional<ProxyResolution> proxy_resolution_from_lua_object(const sol::object& value) {
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
        const auto packages = string_array_from_lua_object(packagesObject);
        if (!packages.has_value()) {
            return std::nullopt;
        }
        resolution.packages = packages.value();
    }

    if (const sol::optional<std::string> localPath = table["localPath"]; localPath.has_value()) {
        resolution.localPath = localPath.value();
    }

    if (const sol::object flagsObject = table["flags"]; flagsObject.valid() && !flagsObject.is<sol::lua_nil_t>()) {
        const auto flags = string_array_from_lua_object(flagsObject);
        if (!flags.has_value()) {
            return std::nullopt;
        }
        resolution.flags = flags.value();
    }

    return resolution;
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
        "version", &Package::version,
        "sourcePath", &Package::sourcePath,
        "localTarget", &Package::localTarget,
        "flags", &Package::flags
    );

    lua.new_usertype<Request>(
        "Request",
        sol::constructors<Request()>(),
        "action", sol::property(
            [](const Request& request) {
                return static_cast<int>(request.action);
            },
            [](Request& request, int action) {
                request.action = static_cast<ActionType>(action);
            }
        ),
        "system", &Request::system,
        "packages", &Request::packages,
        "flags", &Request::flags,
        "outputFormat", &Request::outputFormat,
        "outputPath", &Request::outputPath,
        "localPath", &Request::localPath,
        "usesLocalTarget", &Request::usesLocalTarget
    );

    lua.new_usertype<PackageInfo>(
        "PackageInfo",
        sol::constructors<PackageInfo()>(),
        "system", &PackageInfo::system,
        "name", &PackageInfo::name,
        "packageId", &PackageInfo::packageId,
        "version", &PackageInfo::version,
        "latestVersion", &PackageInfo::latestVersion,
        "status", &PackageInfo::status,
        "installed", &PackageInfo::installed,
        "summary", &PackageInfo::summary,
        "description", &PackageInfo::description,
        "homepage", &PackageInfo::homepage,
        "documentation", &PackageInfo::documentation,
        "sourceUrl", &PackageInfo::sourceUrl,
        "repository", &PackageInfo::repository,
        "channel", &PackageInfo::channel,
        "section", &PackageInfo::section,
        "packageType", &PackageInfo::packageType,
        "type", &PackageInfo::packageType,
        "architecture", &PackageInfo::architecture,
        "license", &PackageInfo::license,
        "author", &PackageInfo::author,
        "maintainer", &PackageInfo::maintainer,
        "email", &PackageInfo::email,
        "publishedAt", &PackageInfo::publishedAt,
        "updatedAt", &PackageInfo::updatedAt,
        "size", &PackageInfo::size,
        "installedSize", &PackageInfo::installedSize,
        "dependencies", &PackageInfo::dependencies,
        "optionalDependencies", &PackageInfo::optionalDependencies,
        "provides", &PackageInfo::provides,
        "conflicts", &PackageInfo::conflicts,
        "replaces", &PackageInfo::replaces,
        "binaries", &PackageInfo::binaries,
		"tags", &PackageInfo::tags
    );

    lua.new_usertype<ExecResult>(
        "ExecResult",
        sol::constructors<ExecResult()>(),
        "success", &ExecResult::success,
        "exitCode", &ExecResult::exitCode,
        "stdout", &ExecResult::stdoutText,
        "stderr", &ExecResult::stderrText
    );
}

void log_lua_error(Logger& logger, const std::string& scope, const std::string& message) {
    logger.emit(OutputAction::LOG, OutputContext{.level = spdlog::level::err, .message = message, .source = "lua", .scope = scope});
}

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

std::string value_to_string(const sol::object& value) {
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

std::string escape_shell_double_quotes(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '"' || c == '$' || c == '`') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return escaped;
}

sol::table make_string_array_table(sol::state& lua, const std::vector<std::string>& values) {
    sol::table table = lua.create_table(static_cast<int>(values.size()), 0);
    for (std::size_t index = 0; index < values.size(); ++index) {
        table[static_cast<int>(index + 1)] = values[index];
    }
    return table;
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
			fields.emplace_back(key.as<std::string>(), value_to_string(value));
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

sol::table make_repository_entry_table(sol::state& lua, const RepositoryEntry& repository) {
    sol::table entry = lua.create_table();
    entry["id"] = repository.id;
    entry["url"] = repository.url;
    entry["priority"] = repository.priority;
    entry["enabled"] = repository.enabled;
    entry["type"] = repository.type;

    sol::table auth = lua.create_table();
    auth["type"] = to_string(repository.auth.type);
    if (!repository.auth.username.empty()) {
        auth["username"] = repository.auth.username;
    }
    if (!repository.auth.password.empty()) {
        auth["password"] = repository.auth.password;
    }
    if (!repository.auth.token.empty()) {
        auth["token"] = repository.auth.token;
    }
    if (!repository.auth.sshKey.empty()) {
        auth["sshKey"] = repository.auth.sshKey;
    }
    if (!repository.auth.headerName.empty()) {
        auth["headerName"] = repository.auth.headerName;
    }
    entry["auth"] = auth;

    sol::table validation = lua.create_table();
    validation["checksum"] = to_string(repository.validation.checksum);
    validation["tlsVerify"] = repository.validation.tlsVerify;
    entry["validation"] = validation;

    sol::table scope = lua.create_table();
    scope["include"] = make_string_array_table(lua, repository.scope.include);
    scope["exclude"] = make_string_array_table(lua, repository.scope.exclude);
    entry["scope"] = scope;

    for (const auto& [key, value] : repository.extras) {
        std::visit([&](const auto& item) {
            using ValueType = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<ValueType, std::vector<std::string>>) {
                entry[key] = make_string_array_table(lua, item);
            } else {
                entry[key] = item;
            }
        }, value);
    }

    return entry;
}

}  // namespace

LuaBridge::LuaBridge(const std::string& scriptPath, const ReqPackConfig& config)
    : m_scriptPath(scriptPath), m_logger(Logger::instance()), m_config(config) {
    m_lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);
    register_types(m_lua);
    register_context_types();
    register_reqpack_namespace();

    const std::filesystem::path resolvedScriptPath(scriptPath);
    m_pluginDirectory = resolvedScriptPath.parent_path().string();
    m_pluginId = resolvedScriptPath.stem().string();
    m_bootstrapPath = (resolvedScriptPath.parent_path() / "bootstrap.lua").string();

    m_lua["REQPACK_PLUGIN_ID"] = m_pluginId;
    m_lua["REQPACK_PLUGIN_DIR"] = m_pluginDirectory;
    m_lua["REQPACK_PLUGIN_SCRIPT"] = m_scriptPath;
    m_lua["REQPACK_PLUGIN_BOOTSTRAP"] = m_bootstrapPath;
    m_lua["print"] = [this](sol::variadic_args args) {
        std::string message;
        bool first = true;
        for (const sol::object& argument : args) {
            if (!first) {
                message += "\t";
            }
            first = false;
            message += value_to_string(argument);
        }

        m_logger.stdout(message, "lua", m_pluginId);
    };

    if (std::filesystem::exists(m_bootstrapPath) && !execute_file(m_lua, m_logger, m_bootstrapPath)) {
        return;
    }

    if (!execute_file(m_lua, m_logger, m_scriptPath)) {
        return;
    }

    m_pluginTable = m_lua["plugin"];
    if (m_pluginTable.valid()) {
        sol::protected_function getName = m_pluginTable["getName"];
        if (getName.valid()) {
            auto result = getName();
            if (result.valid() && result.return_count() > 0) {
                m_name = result.get<std::string>();
            }
        }

        sol::protected_function getVersion = m_pluginTable["getVersion"];
        if (getVersion.valid()) {
            auto result = getVersion();
            if (result.valid() && result.return_count() > 0) {
                m_version = result.get<std::string>();
            }
        }

        sol::protected_function getSecurityMetadata = m_pluginTable["getSecurityMetadata"];
        if (getSecurityMetadata.valid()) {
            auto result = getSecurityMetadata();
            if (result.valid() && result.return_count() > 0) {
                const sol::object value = result.get<sol::object>();
                if (value.valid() && value.get_type() == sol::type::table) {
                    const sol::table metadata = value.as<sol::table>();
                    PluginSecurityMetadata parsed;
                    bool hasAnyField = false;

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

                    if (hasAnyField) {
                        m_securityMetadata = parsed;
                    }
                }
            }
        }

        // Read optional plugin.fileExtensions table.
        const sol::object fileExtObj = m_pluginTable["fileExtensions"];
        if (fileExtObj.valid() && fileExtObj.get_type() == sol::type::table) {
            const sol::table extTable = fileExtObj.as<sol::table>();
            extTable.for_each([this](const sol::object&, const sol::object& val) {
                if (val.valid() && val.get_type() == sol::type::string) {
                    m_fileExtensions.push_back(val.as<std::string>());
                }
            });
        }
    }

    if (m_name.empty()) {
        m_name = m_pluginId;
    }
    if (m_version.empty()) {
        m_version = "0.0.0";
    }
}

void LuaBridge::register_context_types() {
    m_lua.new_usertype<PluginCallContext>(
        "PluginCallContext",
        "flags", sol::readonly_property([](const PluginCallContext& context) {
            return context.flags;
        }),
        "plugin", sol::readonly_property([this](const PluginCallContext& context) {
            sol::table plugin = m_lua.create_table();
            plugin["id"] = context.pluginId;
            plugin["dir"] = context.pluginDirectory;
            plugin["script"] = context.scriptPath;
            plugin["bootstrap"] = context.bootstrapPath;
            return plugin;
        }),
        "repositories", sol::readonly_property([this](const PluginCallContext& context) {
            sol::table repositories = m_lua.create_table(static_cast<int>(context.repositories.size()), 0);
            for (std::size_t index = 0; index < context.repositories.size(); ++index) {
                repositories[static_cast<int>(index + 1)] = make_repository_entry_table(m_lua, context.repositories[index]);
            }
            return repositories;
        }),
        "proxy", sol::readonly_property([this](const PluginCallContext& context) {
            if (!context.proxy.has_value()) {
                return sol::make_object(m_lua, sol::lua_nil);
            }

            sol::table proxy = m_lua.create_table();
            if (!context.proxy->defaultTarget.empty()) {
                proxy["default"] = context.proxy->defaultTarget;
            }
            proxy["targets"] = make_string_array_table(m_lua, context.proxy->targets);

            sol::table options = m_lua.create_table();
            for (const auto& [key, value] : context.proxy->options) {
                options[key] = value;
            }
            proxy["options"] = options;

            return sol::make_object(m_lua, proxy);
        }),
        "log", sol::readonly_property([this](const PluginCallContext& context) {
            sol::table log = m_lua.create_table();
			const PluginCallContext* contextPtr = &context;
			log.set_function("debug", [contextPtr](const std::string& message) {
				if (contextPtr->host != nullptr) contextPtr->host->logDebug(contextPtr->pluginId, message);
			});
			log.set_function("info", [contextPtr](const std::string& message) {
				if (contextPtr->host != nullptr) contextPtr->host->logInfo(contextPtr->pluginId, message);
			});
			log.set_function("warn", [contextPtr](const std::string& message) {
				if (contextPtr->host != nullptr) contextPtr->host->logWarn(contextPtr->pluginId, message);
			});
			log.set_function("error", [contextPtr](const std::string& message) {
				if (contextPtr->host != nullptr) contextPtr->host->logError(contextPtr->pluginId, message);
			});
            return log;
        }),
        "tx", sol::readonly_property([this](const PluginCallContext& context) {
            sol::table tx = m_lua.create_table();
			const PluginCallContext* contextPtr = &context;
			tx.set_function("status", [contextPtr](int code) {
				if (contextPtr->host != nullptr) contextPtr->host->emitStatus(contextPtr->currentItemId.empty() ? contextPtr->pluginId : contextPtr->currentItemId, code);
			});
			tx.set_function("progress", [contextPtr](sol::object payload) {
                if (const std::optional<DisplayProgressMetrics> metrics = progress_metrics_from_lua_object(payload); metrics.has_value()) {
					if (contextPtr->host != nullptr) {
						contextPtr->host->emitProgress(contextPtr->currentItemId.empty() ? contextPtr->pluginId : contextPtr->currentItemId, metrics.value());
					}
                }
            });
			tx.set_function("begin_step", [contextPtr](const std::string& label) {
				if (contextPtr->host != nullptr) contextPtr->host->emitBeginStep(contextPtr->currentItemId.empty() ? contextPtr->pluginId : contextPtr->currentItemId, label);
			});
			tx.set_function("commit", [contextPtr]() {
				if (contextPtr->host != nullptr) contextPtr->host->emitCommit(contextPtr->currentItemId.empty() ? contextPtr->pluginId : contextPtr->currentItemId);
			});
			tx.set_function("success", [contextPtr]() {
				if (contextPtr->host != nullptr) contextPtr->host->emitSuccess(contextPtr->currentItemId.empty() ? contextPtr->pluginId : contextPtr->currentItemId);
			});
			tx.set_function("failed", [contextPtr](const std::string& message) {
				if (contextPtr->host != nullptr) contextPtr->host->emitFailure(contextPtr->currentItemId.empty() ? contextPtr->pluginId : contextPtr->currentItemId, message);
			});
            return tx;
        }),
        "events", sol::readonly_property([this](const PluginCallContext& context) {
            sol::table events = m_lua.create_table();
            const std::array<const char*, 8> names{"installed", "deleted", "updated", "listed", "searched", "informed", "outdated", "unavailable"};
			const PluginCallContext* contextPtr = &context;
            for (const char* name : names) {
				events.set_function(name, [this, contextPtr, name](sol::object payload) {
					if (contextPtr->host != nullptr) {
						contextPtr->host->emitEvent(contextPtr->currentItemId.empty() ? contextPtr->pluginId : contextPtr->currentItemId, name, serializeLuaPayload(payload));
					}
                });
            }
            return events;
        }),
        "artifacts", sol::readonly_property([this](const PluginCallContext& context) {
            sol::table artifacts = m_lua.create_table();
			const PluginCallContext* contextPtr = &context;
            artifacts.set_function("register", [this, contextPtr](sol::object payload) {
				if (contextPtr->host != nullptr) contextPtr->host->registerArtifact(contextPtr->pluginId, serializeLuaPayload(payload));
            });
            return artifacts;
        }),
        "exec", sol::readonly_property([this](const PluginCallContext& context) {
            sol::table exec = m_lua.create_table();
			const PluginCallContext* contextPtr = &context;
            exec.set_function("run", sol::overload(
				[contextPtr](const std::string& command) {
					return contextPtr->host != nullptr ? contextPtr->host->execute(contextPtr->pluginId, command) : ExecResult{};
                },
				[this, contextPtr](const std::string& command, const sol::object& rules) {
                    return run_plugin_command(
                        m_logger,
						contextPtr->currentItemId.empty() ? contextPtr->pluginId : contextPtr->currentItemId,
						contextPtr->pluginId,
                        command,
                        rules,
						hasSilentRuntimeFlag(contextPtr->flags)
                    );
                }
            ));
            return exec;
        }),
        "fs", sol::readonly_property([this](const PluginCallContext& context) {
            sol::table fs = m_lua.create_table();
			const PluginCallContext* contextPtr = &context;
            fs.set_function("get_tmp_dir", [contextPtr]() {
				return contextPtr->host != nullptr ? contextPtr->host->createTempDirectory(contextPtr->pluginId) : std::string{};
            });
            return fs;
        }),
        "net", sol::readonly_property([this](const PluginCallContext& context) {
            sol::table net = m_lua.create_table();
			const PluginCallContext* contextPtr = &context;
            net.set_function("download", [contextPtr](const std::string& url, const std::string& destinationPath) {
				const DownloadResult result = contextPtr->host != nullptr ? contextPtr->host->download(contextPtr->pluginId, url, destinationPath) : DownloadResult{};
                return result.success;
            });
            return net;
        })
    );
}

void LuaBridge::register_reqpack_namespace() {
    sol::table reqpack = m_lua.create_named_table("reqpack");
    sol::table exec = m_lua.create_table();
    exec.set_function("run", [this](const std::string& command) {
        return runCommand(command, m_silentRuntimeOutput.load());
    });
    reqpack["exec"] = exec;
}

bool LuaBridge::hasSilentRuntimeFlag(const std::vector<std::string>& flags) const {
    return std::find(flags.begin(), flags.end(), SILENT_RUNTIME_FLAG) != flags.end();
}

bool LuaBridge::validatePluginContract() const {
    if (!m_pluginTable.valid()) {
        log_lua_error(m_logger, m_pluginId, "[Lua API Error] plugin table is required.");
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
            log_lua_error(m_logger, m_pluginId, std::string("[Lua API Error] ") + method + " is required.");
            return false;
        }
    }

    const std::array<const char*, 2> requiredQueryMethods{"search", "info"};
    for (const char* method : requiredQueryMethods) {
		sol::protected_function function = m_pluginTable[method];
		if (!function.valid()) {
            log_lua_error(m_logger, m_pluginId, std::string("[Lua API Error] ") + method + " is required.");
            return false;
        }
    }

    return true;
}

PluginCallContext LuaBridge::makeContext(const std::vector<std::string>& flags) const {
    return PluginCallContext{
        .pluginId = m_pluginId,
        .pluginDirectory = m_pluginDirectory,
        .scriptPath = m_scriptPath,
        .bootstrapPath = m_bootstrapPath,
        .flags = flags,
		.host = const_cast<LuaBridge*>(this),
		.proxy = proxy_config_for_system(m_config, m_pluginId),
		.repositories = repositories_for_ecosystem(m_config, m_pluginId)
    };
}

bool LuaBridge::init() {
    if (!validatePluginContract()) {
        return false;
    }

    sol::protected_function bootstrap = m_lua["bootstrap"];
    if (bootstrap.valid()) {
        auto bootstrapResult = bootstrap();
        if (!bootstrapResult.valid()) {
            sol::error err = bootstrapResult;
            log_lua_error(m_logger, m_pluginId, std::string("[Lua Exec Error] bootstrap(): ") + err.what());
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
            log_lua_error(m_logger, m_pluginId, std::string("[Lua Exec Error] init(): ") + err.what());
            return false;
        }
        return result.return_count() == 0 ? true : result.get<bool>();
    }

    return true;
}

bool LuaBridge::shutdown() {
    sol::protected_function luaShutdown = m_pluginTable["shutdown"];
    const bool shutdownOk = luaShutdown.valid() ? [&]() {
        auto result = luaShutdown();
        return result.valid() ? (result.return_count() == 0 ? true : result.get<bool>()) : false;
    }() : true;

    for (const std::string& path : m_tempDirectories) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    m_tempDirectories.clear();

    return shutdownOk;
}

std::vector<Package> LuaBridge::getRequirements() {
	if (!m_pluginTable.valid()) {
		return {};
	}
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

std::vector<std::string> LuaBridge::getCategories() {
	if (!m_pluginTable.valid()) {
		return {};
	}
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
	if (!m_pluginTable.valid()) {
		return packages;
	}
    sol::protected_function func = m_pluginTable["getMissingPackages"];
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
            if (const auto missingPackages = packages_from_lua_object(value)) {
                return missingPackages.value();
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
    sol::protected_function func = m_pluginTable["resolvePackage"];
    return func.valid();
}

bool LuaBridge::supportsProxyResolution() const {
    sol::protected_function func = m_pluginTable["resolveProxyRequest"];
    return func.valid();
}

std::vector<PluginEventRecord> LuaBridge::takeRecentEvents() {
    std::vector<PluginEventRecord> events = std::move(m_recentEvents);
    m_recentEvents.clear();
    return events;
}

bool LuaBridge::install(const PluginCallContext& context, const std::vector<Package>& packages) {
    m_recentEvents.clear();
    sol::protected_function func = m_pluginTable["install"];
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
    m_recentEvents.clear();
    sol::protected_function func = m_pluginTable["installLocal"];
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
    m_recentEvents.clear();
    sol::protected_function func = m_pluginTable["remove"];
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
    m_recentEvents.clear();
    sol::protected_function func = m_pluginTable["update"];
    if (!func.valid()) {
        return false;
    }

    auto result = packages.empty() ? func(context, sol::lua_nil) : func(context, packages);
    if (!result.valid()) {
        sol::error err = result;
        log_lua_error(m_logger, m_pluginId, std::string("Lua Error (update): ") + err.what());
        return false;
    }
    return result.return_count() == 0 ? true : result.get<bool>();
}

std::vector<PackageInfo> LuaBridge::packageInfoListFromObject(const sol::object& value) const {
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

PackageInfo LuaBridge::packageInfoFromObject(const sol::object& value) const {
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

std::vector<PackageInfo> LuaBridge::list(const PluginCallContext& context) {
    sol::protected_function func = m_pluginTable["list"];
    if (!func.valid()) {
        return {};
    }

    const bool silentRuntime = hasSilentRuntimeFlag(context.flags);
    m_silentRuntimeOutput.store(silentRuntime);
    auto result = func(context);
    m_silentRuntimeOutput.store(false);
    if (!result.valid()) {
        sol::error err = result;
        log_lua_error(m_logger, m_pluginId, std::string("Lua Error (list): ") + err.what());
        return {};
    }
    if (result.return_count() == 0) {
        return {};
    }
    return packageInfoListFromObject(result.get<sol::object>());
}

std::vector<PackageInfo> LuaBridge::outdated(const PluginCallContext& context) {
    sol::protected_function func = m_pluginTable["outdated"];
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
    return packageInfoListFromObject(result.get<sol::object>());
}

std::vector<PackageInfo> LuaBridge::search(const PluginCallContext& context, const std::string& prompt) {
    sol::protected_function func = m_pluginTable["search"];
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
    return packageInfoListFromObject(result.get<sol::object>());
}

PackageInfo LuaBridge::info(const PluginCallContext& context, const std::string& packageName) {
    sol::protected_function func = m_pluginTable["info"];
    if (!func.valid()) {
        return {};
    }

    const bool silentRuntime = hasSilentRuntimeFlag(context.flags);
    m_silentRuntimeOutput.store(silentRuntime);
    auto result = func(context, packageName);
    m_silentRuntimeOutput.store(false);
    if (!result.valid()) {
        sol::error err = result;
        log_lua_error(m_logger, m_pluginId, std::string("Lua Error (info): ") + err.what());
        return {};
    }
    if (result.return_count() == 0) {
        return {};
    }
    return packageInfoFromObject(result.get<sol::object>());
}

std::optional<Package> LuaBridge::resolvePackage(const PluginCallContext& context, const Package& package) {
    sol::protected_function func = m_pluginTable["resolvePackage"];
    if (!func.valid()) {
        return std::nullopt;
    }

    const bool silentRuntime = hasSilentRuntimeFlag(context.flags);
    m_silentRuntimeOutput.store(silentRuntime);
    auto result = func(context, package);
    m_silentRuntimeOutput.store(false);
    if (!result.valid()) {
        sol::error err = result;
        log_lua_error(m_logger, m_pluginId, std::string("Lua Error (resolvePackage): ") + err.what());
        return std::nullopt;
    }
    if (result.return_count() == 0) {
        return std::nullopt;
    }

    const sol::object value = result.get<sol::object>();
    if (!value.valid()) {
        return std::nullopt;
    }
    if (value.is<sol::lua_nil_t>()) {
        return std::nullopt;
    }
    if (const auto resolved = package_from_lua_object(value)) {
        return resolved.value();
    }

    log_lua_error(m_logger, m_pluginId, "[Lua API Error] resolvePackage(context, package) must return a package or nil.");
    return std::nullopt;
}

std::optional<ProxyResolution> LuaBridge::resolveProxyRequest(const PluginCallContext& context, const Request& request) {
    sol::protected_function func = m_pluginTable["resolveProxyRequest"];
    if (!func.valid()) {
        return std::nullopt;
    }

    const bool silentRuntime = hasSilentRuntimeFlag(context.flags);
    m_silentRuntimeOutput.store(silentRuntime);
    auto result = func(context, request);
    m_silentRuntimeOutput.store(false);
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
    if (const auto resolved = proxy_resolution_from_lua_object(value)) {
        return resolved.value();
    }

    log_lua_error(m_logger, m_pluginId, "[Lua API Error] resolveProxyRequest(context, request) must return a proxy resolution table or nil.");
    return std::nullopt;
}

std::string LuaBridge::serializeLuaPayload(const sol::object& value) const {
    if (!value.valid()) {
        return "null";
    }
    if (value.is<std::string>() || value.is<bool>() || value.is<int>() || value.is<double>()) {
        return value_to_string(value);
    }
    if (value.get_type() != sol::type::table) {
        return "<lua-value>";
    }

    std::ostringstream stream;
    stream << '{';
    bool first = true;
    for (const auto& [key, entry] : value.as<sol::table>()) {
        if (!first) {
            stream << ", ";
        }
        first = false;
        stream << value_to_string(key) << '=' << value_to_string(entry);
    }
    stream << '}';
    return stream.str();
}

ExecResult LuaBridge::runCommand(const std::string& command) const {
    return runCommand(command, m_silentRuntimeOutput.load());
}

ExecResult LuaBridge::runCommand(const std::string& command, const bool silent) const {
    return run_plugin_command(m_logger, m_pluginId, m_pluginId, command, silent);
}

DownloadResult LuaBridge::downloadToPath(const std::string& url, const std::string& destinationPath) {
    Downloader downloader(nullptr, m_config);
    const std::filesystem::path sourcePath = [url]() {
        if (url.rfind("file://", 0) == 0) {
            return std::filesystem::path(url.substr(7));
        }
        return std::filesystem::path(url);
    }();
    const std::string suffix = generic_archive_suffix(sourcePath);
    const std::filesystem::path targetPath = suffix.empty() ? std::filesystem::path(destinationPath)
                                                             : std::filesystem::path(destinationPath + suffix);
    if (!downloader.download(url, targetPath.string())) {
        return {};
    }

    DownloadResult result;
    result.success = true;
    result.resolvedPath = destinationPath;

    try {
        if (extract_archive_in_place(targetPath)) {
            if (targetPath != std::filesystem::path(destinationPath)) {
                std::error_code error;
                std::filesystem::remove_all(destinationPath, error);
                std::filesystem::rename(targetPath, destinationPath, error);
                if (error) {
                    return {};
                }
            }
            result.resolvedPath = destinationPath;
        } else if (targetPath != std::filesystem::path(destinationPath)) {
            std::error_code error;
            std::filesystem::rename(targetPath, destinationPath, error);
            if (error) {
                return {};
            }
        }
    } catch (...) {
        return {};
    }

    return result;
}

void LuaBridge::logDebug(const std::string& pluginId, const std::string& message) {
	if (m_silentRuntimeOutput.load()) {
		return;
	}
    m_logger.emit(OutputAction::LOG, OutputContext{.level = spdlog::level::debug, .message = message, .source = "plugin", .scope = pluginId});
}

void LuaBridge::logInfo(const std::string& pluginId, const std::string& message) {
	if (m_silentRuntimeOutput.load()) {
		return;
	}
    m_logger.emit(OutputAction::LOG, OutputContext{.level = spdlog::level::info, .message = message, .source = "plugin", .scope = pluginId});
}

void LuaBridge::logWarn(const std::string& pluginId, const std::string& message) {
	if (m_silentRuntimeOutput.load()) {
		return;
	}
    m_logger.emit(OutputAction::LOG, OutputContext{.level = spdlog::level::warn, .message = message, .source = "plugin", .scope = pluginId});
}

void LuaBridge::logError(const std::string& pluginId, const std::string& message) {
	if (m_silentRuntimeOutput.load()) {
		return;
	}
    m_logger.emit(OutputAction::LOG, OutputContext{.level = spdlog::level::err, .message = message, .source = "plugin", .scope = pluginId});
}

void LuaBridge::emitStatus(const std::string& pluginId, int statusCode) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    const bool hasItemId = pluginId.find(':') != std::string::npos;
    m_logger.emit(OutputAction::PLUGIN_STATUS, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = m_pluginId, .statusCode = statusCode});
}

void LuaBridge::emitProgress(const std::string& pluginId, const DisplayProgressMetrics& metrics) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    const bool hasItemId = pluginId.find(':') != std::string::npos;
    const DisplayProgressMetrics normalized = canonicalize_progress_metrics(metrics);
    if (!normalized.percent.has_value() && !normalized.currentBytes.has_value() && !normalized.totalBytes.has_value() && !normalized.bytesPerSecond.has_value()) {
        return;
    }
    m_logger.emit(OutputAction::PLUGIN_PROGRESS, OutputContext{
        .source = hasItemId ? pluginId : "plugin",
        .scope = m_pluginId,
        .progressPercent = normalized.percent,
        .currentBytes = normalized.currentBytes,
        .totalBytes = normalized.totalBytes,
        .bytesPerSecond = normalized.bytesPerSecond,
    });
}

void LuaBridge::emitBeginStep(const std::string& pluginId, const std::string& label) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    const bool hasItemId = pluginId.find(':') != std::string::npos;
    m_logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = m_pluginId, .eventName = "begin_step", .payload = label});
}

void LuaBridge::emitCommit(const std::string& pluginId) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    const bool hasItemId = pluginId.find(':') != std::string::npos;
    m_logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = m_pluginId, .eventName = "commit", .payload = "committed"});
}

void LuaBridge::emitSuccess(const std::string& pluginId) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    const bool hasItemId = pluginId.find(':') != std::string::npos;
    m_logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = m_pluginId, .eventName = "success", .payload = "ok"});
}

void LuaBridge::emitFailure(const std::string& pluginId, const std::string& message) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    const bool hasItemId = pluginId.find(':') != std::string::npos;
    m_logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = m_pluginId, .eventName = "failed", .payload = message});
}

void LuaBridge::emitEvent(const std::string& pluginId, const std::string& eventName, const std::string& payload) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    const bool hasItemId = pluginId.find(':') != std::string::npos;
    m_recentEvents.push_back(PluginEventRecord{.name = eventName, .payload = payload});
    m_logger.emit(OutputAction::PLUGIN_EVENT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = m_pluginId, .eventName = eventName, .payload = payload});
}

void LuaBridge::registerArtifact(const std::string& pluginId, const std::string& payload) {
    if (m_silentRuntimeOutput.load()) {
        return;
    }
    const bool hasItemId = pluginId.find(':') != std::string::npos;
    m_logger.emit(OutputAction::PLUGIN_ARTIFACT, OutputContext{.source = hasItemId ? pluginId : "plugin", .scope = m_pluginId, .payload = payload});
}

ExecResult LuaBridge::execute(const std::string& pluginId, const std::string& command) {
    (void)pluginId;
    return runCommand(command);
}

std::string LuaBridge::createTempDirectory(const std::string& pluginId) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / ("reqpack-" + pluginId + "-XXXXXX");
    std::string templateString = tempDir.string();
    std::vector<char> buffer(templateString.begin(), templateString.end());
    buffer.push_back('\0');
    char* created = mkdtemp(buffer.data());
    if (created == nullptr) {
        return {};
    }
    m_tempDirectories.emplace_back(created);
    return created;
}

DownloadResult LuaBridge::download(const std::string& pluginId, const std::string& url, const std::string& destinationPath) {
    (void)pluginId;
    return downloadToPath(url, destinationPath);
}
