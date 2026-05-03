#include "core/configuration.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::optional<bool> bool_from_string(const std::string& value) {
    const std::string normalized = to_lower(value);
    if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off") {
        return false;
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> passwd_home_from_user(const std::string& username) {
    if (username.empty()) {
        return std::nullopt;
    }

    if (passwd* entry = getpwnam(username.c_str())) {
        if (entry->pw_dir != nullptr && std::string(entry->pw_dir).size() > 0) {
            return std::filesystem::path(entry->pw_dir);
        }
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> passwd_home_from_uid(uid_t uid) {
    if (passwd* entry = getpwuid(uid)) {
        if (entry->pw_dir != nullptr && std::string(entry->pw_dir).size() > 0) {
            return std::filesystem::path(entry->pw_dir);
        }
    }

    return std::nullopt;
}

std::filesystem::path invoking_user_home_directory() {
    const char* sudoUser = std::getenv("SUDO_USER");
    if (sudoUser != nullptr && std::string(sudoUser).size() > 0) {
        if (const auto home = passwd_home_from_user(sudoUser)) {
            return home.value();
        }
    }

    const char* sudoUid = std::getenv("SUDO_UID");
    if (sudoUid != nullptr && std::string(sudoUid).size() > 0) {
        try {
            if (const auto home = passwd_home_from_uid(static_cast<uid_t>(std::stoul(sudoUid)))) {
                return home.value();
            }
        } catch (...) {
        }
    }

    const char* home = std::getenv("HOME");
    if (home != nullptr && std::string(home).size() > 0) {
        return std::filesystem::path(home);
    }

    if (const auto passwdHome = passwd_home_from_uid(getuid())) {
        return passwdHome.value();
    }

    return std::filesystem::current_path();
}

std::filesystem::path expand_user_path(const std::filesystem::path& path) {
    const std::string raw = path.string();
    if (raw.empty() || raw.front() != '~') {
        return path;
    }

    const std::filesystem::path home = invoking_user_home_directory();
    if (raw == "~") {
        return home;
    }
    if (raw.rfind("~/", 0) == 0) {
        return home / raw.substr(2);
    }

    return path;
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
        } else {
            continue;
        }

        if (entry.source.empty()) {
            continue;
        }

        if (entry.alias) {
            entry.source = to_lower(entry.source);
        } else {
            entry.source = expand_user_path(entry.source).string();
        }

        sources[to_lower(key.as<std::string>())] = std::move(entry);
    }

    return sources;
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

std::map<std::string, std::string> load_string_map(const sol::object& object) {
    std::map<std::string, std::string> values;
    if (!object.valid() || object.get_type() != sol::type::table) {
        return values;
    }

    for (const auto& [key, value] : object.as<sol::table>()) {
        if (key.get_type() != sol::type::string || value.get_type() != sol::type::string) {
            continue;
        }

        values[to_lower(key.as<std::string>())] = value.as<std::string>();
    }

    return values;
}

template <typename Enum>
void assign_if_present(const sol::table& table, const char* key, std::optional<Enum> (*converter)(const std::string&), Enum& target);

template <typename T>
void assign_if_present(const sol::table& table, const char* key, T& target);

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
                gateway.backends.push_back(to_lower(backend));
            }
        }

        values[to_lower(key.as<std::string>())] = std::move(gateway);
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

        values[to_lower(key.as<std::string>())] = std::move(backend);
    }

    return values;
}

void merge_registry_sources(RegistrySourceMap& target, const RegistrySourceMap& source) {
    for (const auto& [name, entry] : source) {
        target[name] = entry;
    }
}

template <typename Enum>
void assign_if_present(const sol::table& table, const char* key, std::optional<Enum> (*converter)(const std::string&), Enum& target) {
    const sol::optional<std::string> value = table[key];
    if (!value.has_value()) {
        return;
    }

    const std::optional<Enum> converted = converter(value.value());
    if (converted.has_value()) {
        target = converted.value();
    }
}

template <typename T>
void assign_if_present(const sol::table& table, const char* key, T& target) {
    const sol::optional<T> value = table[key];
    if (value.has_value()) {
        target = value.value();
    }
}

}  // namespace

std::optional<SeverityLevel> severity_level_from_string(const std::string& severity) {
    const std::string normalized = to_lower(severity);
    if (normalized == "critical") {
        return SeverityLevel::CRITICAL;
    }
    if (normalized == "high") {
        return SeverityLevel::HIGH;
    }
    if (normalized == "medium") {
        return SeverityLevel::MEDIUM;
    }
    if (normalized == "low") {
        return SeverityLevel::LOW;
    }
    if (normalized == "unassigned") {
        return SeverityLevel::UNASSIGNED;
    }

    return std::nullopt;
}

std::optional<LogLevel> log_level_from_string(const std::string& level) {
    const std::string normalized = to_lower(level);
    if (normalized == "trace") {
        return LogLevel::TRACE;
    }
    if (normalized == "debug") {
        return LogLevel::DEBUG;
    }
    if (normalized == "info") {
        return LogLevel::INFO;
    }
    if (normalized == "warn" || normalized == "warning") {
        return LogLevel::WARN;
    }
    if (normalized == "error") {
        return LogLevel::ERROR;
    }
    if (normalized == "critical") {
        return LogLevel::CRITICAL;
    }

    return std::nullopt;
}

std::optional<ReportFormat> report_format_from_string(const std::string& format) {
    const std::string normalized = to_lower(format);
    if (normalized == "none") {
        return ReportFormat::NONE;
    }
    if (normalized == "json") {
        return ReportFormat::JSON;
    }
    if (normalized == "cyclonedx") {
        return ReportFormat::CYCLONEDX;
    }

    return std::nullopt;
}

std::optional<UnsafeAction> unsafe_action_from_string(const std::string& action) {
    const std::string normalized = to_lower(action);
    if (normalized == "continue") {
        return UnsafeAction::CONTINUE;
    }
    if (normalized == "prompt" || normalized == "ask") {
        return UnsafeAction::PROMPT;
    }
    if (normalized == "abort" || normalized == "fail") {
        return UnsafeAction::ABORT;
    }

    return std::nullopt;
}

std::optional<OsvRefreshMode> osv_refresh_mode_from_string(const std::string& mode) {
    const std::string normalized = to_lower(mode);
    if (normalized == "manual") {
        return OsvRefreshMode::MANUAL;
    }
    if (normalized == "periodic") {
        return OsvRefreshMode::PERIODIC;
    }
    if (normalized == "always") {
        return OsvRefreshMode::ALWAYS;
    }

    return std::nullopt;
}

std::optional<SbomOutputFormat> sbom_output_format_from_string(const std::string& format) {
    const std::string normalized = to_lower(format);
    if (normalized == "table") {
        return SbomOutputFormat::TABLE;
    }
    if (normalized == "json") {
        return SbomOutputFormat::JSON;
    }
    if (normalized == "cyclonedx-json" || normalized == "cyclonedx") {
        return SbomOutputFormat::CYCLONEDX_JSON;
    }

    return std::nullopt;
}

std::optional<AuditOutputFormat> audit_output_format_from_string(const std::string& format) {
    const std::string normalized = to_lower(format);
    if (normalized == "table") {
        return AuditOutputFormat::TABLE;
    }
    if (normalized == "json") {
        return AuditOutputFormat::JSON;
    }
    if (normalized == "cyclonedx-vex-json" || normalized == "cyclonedx-vex" || normalized == "cyclonedx") {
        return AuditOutputFormat::CYCLONEDX_VEX_JSON;
    }
    if (normalized == "sarif") {
        return AuditOutputFormat::SARIF;
    }

    return std::nullopt;
}

std::optional<DisplayRenderer> display_renderer_from_string(const std::string& renderer) {
    const std::string normalized = to_lower(renderer);
    if (normalized == "plain") {
        return DisplayRenderer::PLAIN;
    }
    if (normalized == "color" || normalized == "colour") {
        return DisplayRenderer::COLOR;
    }

    return std::nullopt;
}

std::filesystem::path reqpack_home_directory() {
    return invoking_user_home_directory() / ".reqpack";
}

std::filesystem::path default_reqpack_config_path() {
    return reqpack_home_directory() / "config.lua";
}

std::filesystem::path default_reqpack_registry_path() {
    return reqpack_home_directory() / "registry";
}

std::filesystem::path registry_database_directory(const std::filesystem::path& registryPath) {
    const std::filesystem::path resolvedPath = expand_user_path(registryPath);
    if (resolvedPath.has_extension()) {
        return resolvedPath.parent_path();
    }

    return resolvedPath;
}

std::filesystem::path registry_source_file_path(const std::filesystem::path& registryPath) {
    const std::filesystem::path resolvedPath = expand_user_path(registryPath);
    if (resolvedPath.has_extension()) {
        return resolvedPath;
    }

    return resolvedPath / "registry.lua";
}

RegistrySourceMap load_registry_sources_from_lua(const std::filesystem::path& sourcePath) {
    const std::filesystem::path resolvedSourcePath = expand_user_path(sourcePath);
    if (!std::filesystem::exists(resolvedSourcePath)) {
        return {};
    }

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table, sol::lib::string, sol::lib::math);

    sol::load_result loadResult = lua.load_file(resolvedSourcePath.string());
    if (!loadResult.valid()) {
        return {};
    }

    const sol::protected_function_result executionResult = loadResult();
    if (!executionResult.valid()) {
        return {};
    }

    sol::table root;
    if (executionResult.get_type() == sol::type::table) {
        root = executionResult;
    } else {
        const sol::object registryObject = lua["registry"];
        if (registryObject.get_type() != sol::type::table) {
            return {};
        }

        root = registryObject.as<sol::table>();
    }

    const sol::optional<sol::table> sources = root["sources"];
    return load_registry_sources_from_table(sources.has_value() ? sources.value() : root);
}

RegistrySourceMap collect_registry_sources(const ReqPackConfig& config) {
    RegistrySourceMap sources;

    for (const auto& [name, source] : config.downloader.pluginSources) {
        sources[to_lower(name)] = RegistrySourceEntry{.source = source};
    }

    merge_registry_sources(sources, config.registry.sources);
    merge_registry_sources(sources, load_registry_sources_from_lua(registry_source_file_path(config.registry.databasePath)));

    if (!config.registry.overlayPath.empty()) {
        merge_registry_sources(sources, load_registry_sources_from_lua(config.registry.overlayPath));
    }

    return sources;
}

ReqPackConfig load_config_from_lua(const std::filesystem::path& configPath, const ReqPackConfig& fallback) {
    ReqPackConfig config = fallback;
    const std::filesystem::path resolvedConfigPath = expand_user_path(configPath);
    if (!std::filesystem::exists(resolvedConfigPath)) {
        return config;
    }

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table, sol::lib::string, sol::lib::math);

    sol::load_result loadResult = lua.load_file(resolvedConfigPath.string());
    if (!loadResult.valid()) {
        return config;
    }

    const sol::protected_function_result executionResult = loadResult();
    if (!executionResult.valid()) {
        return config;
    }

    sol::table root;
    if (executionResult.get_type() == sol::type::table) {
        root = executionResult;
    } else {
        const sol::object configObject = lua["config"];
        if (configObject.get_type() == sol::type::table) {
            root = configObject.as<sol::table>();
        } else {
            return config;
        }
    }

    assign_if_present(root, "applicationName", config.applicationName);
    assign_if_present(root, "version", config.version);

    const sol::optional<sol::table> logging = root["logging"];
    if (logging.has_value()) {
        assign_if_present(logging.value(), "consoleOutput", config.logging.consoleOutput);
        assign_if_present(logging.value(), "fileOutput", config.logging.fileOutput);
        assign_if_present(logging.value(), "filePath", config.logging.filePath);
        assign_if_present(logging.value(), "enableBacktrace", config.logging.enableBacktrace);
        assign_if_present(logging.value(), "backtraceSize", config.logging.backtraceSize);
        assign_if_present(logging.value(), "pattern", config.logging.pattern);
        assign_if_present(logging.value(), "level", log_level_from_string, config.logging.level);
    }
    config.logging.filePath = expand_user_path(config.logging.filePath).string();

    const sol::optional<sol::table> security = root["security"];
    if (security.has_value()) {
        assign_if_present(security.value(), "enabled", config.security.enabled);
        assign_if_present(security.value(), "autoFetch", config.security.autoFetch);
        assign_if_present(security.value(), "runSnykScan", config.security.runSnykScan);
        assign_if_present(security.value(), "runOwaspScan", config.security.runOwaspScan);
        assign_if_present(security.value(), "scoreThreshold", config.security.scoreThreshold);
        assign_if_present(security.value(), "promptOnUnsafe", config.security.promptOnUnsafe);
        assign_if_present(security.value(), "allowUnassigned", config.security.allowUnassigned);
        assign_if_present(security.value(), "defaultGateway", config.security.defaultGateway);
        assign_if_present(security.value(), "cachePath", config.security.cachePath);
        assign_if_present(security.value(), "indexPath", config.security.indexPath);
        assign_if_present(security.value(), "severityThreshold", severity_level_from_string, config.security.severityThreshold);
        assign_if_present(security.value(), "onUnsafe", unsafe_action_from_string, config.security.onUnsafe);
        assign_if_present(security.value(), "osvDatabasePath", config.security.osvDatabasePath);
        assign_if_present(security.value(), "osvFeedUrl", config.security.osvFeedUrl);
        assign_if_present(security.value(), "osvRefreshMode", osv_refresh_mode_from_string, config.security.osvRefreshMode);
        assign_if_present(security.value(), "osvRefreshIntervalSeconds", config.security.osvRefreshIntervalSeconds);
        assign_if_present(security.value(), "osvOverlayPath", config.security.osvOverlayPath);
        assign_if_present(security.value(), "onUnresolvedVersion", unsafe_action_from_string, config.security.onUnresolvedVersion);
        assign_if_present(security.value(), "strictEcosystemMapping", config.security.strictEcosystemMapping);
        assign_if_present(security.value(), "includeWithdrawnInReport", config.security.includeWithdrawnInReport);

        const std::vector<std::string> ignoreIds = load_string_array(security.value()["ignoreVulnerabilityIds"]);
        if (!ignoreIds.empty()) {
            config.security.ignoreVulnerabilityIds = ignoreIds;
        }

        const std::vector<std::string> allowIds = load_string_array(security.value()["allowVulnerabilityIds"]);
        if (!allowIds.empty()) {
            config.security.allowVulnerabilityIds = allowIds;
        }

        const std::map<std::string, std::string> ecosystemMap = load_string_map(security.value()["osvEcosystemMap"]);
        for (const auto& [system, ecosystem] : ecosystemMap) {
            config.security.osvEcosystemMap[system] = ecosystem;
        }

        const std::map<std::string, std::string> securityEcosystemMap = load_string_map(security.value()["ecosystemMap"]);
        for (const auto& [system, ecosystem] : securityEcosystemMap) {
            config.security.ecosystemMap[system] = ecosystem;
        }

        const std::map<std::string, SecurityGatewayConfig> gateways = load_security_gateway_map(security.value()["gateways"]);
        for (const auto& [name, gateway] : gateways) {
            config.security.gateways[name] = gateway;
        }

        const std::map<std::string, SecurityBackendConfig> backends = load_security_backend_map(security.value()["backends"]);
        for (const auto& [name, backend] : backends) {
            config.security.backends[name] = backend;
        }
    }
    const std::string defaultIndexPath = expand_user_path(SecurityConfig{}.indexPath).string();
    const std::string defaultOsvDatabasePath = expand_user_path(SecurityConfig{}.osvDatabasePath).string();
    if (config.security.indexPath == defaultIndexPath && config.security.osvDatabasePath != defaultOsvDatabasePath) {
        config.security.indexPath = config.security.osvDatabasePath;
    }
    config.security.defaultGateway = to_lower(config.security.defaultGateway);
    config.security.cachePath = expand_user_path(config.security.cachePath).string();
    config.security.indexPath = expand_user_path(config.security.indexPath).string();
    config.security.osvDatabasePath = expand_user_path(config.security.osvDatabasePath).string();
    if (!config.security.osvOverlayPath.empty()) {
        config.security.osvOverlayPath = expand_user_path(config.security.osvOverlayPath).string();
    }
    if (!config.security.ecosystemMap.empty()) {
        for (const auto& [system, ecosystem] : config.security.ecosystemMap) {
            config.security.osvEcosystemMap[system] = ecosystem;
        }
    }
    if (!config.security.backends.contains("osv")) {
        SecurityBackendConfig osvBackend;
        osvBackend.feedUrl = config.security.osvFeedUrl;
        osvBackend.refreshMode = config.security.osvRefreshMode;
        osvBackend.refreshIntervalSeconds = config.security.osvRefreshIntervalSeconds;
        osvBackend.overlayPath = config.security.osvOverlayPath;
        config.security.backends["osv"] = std::move(osvBackend);
    } else {
        SecurityBackendConfig& osvBackend = config.security.backends["osv"];
        if (osvBackend.feedUrl.empty()) {
            osvBackend.feedUrl = config.security.osvFeedUrl;
        }
        if (osvBackend.overlayPath.empty()) {
            osvBackend.overlayPath = config.security.osvOverlayPath;
        }
    }

    const sol::optional<sol::table> reports = root["reports"];
    if (reports.has_value()) {
        assign_if_present(reports.value(), "enabled", config.reports.enabled);
        assign_if_present(reports.value(), "outputPath", config.reports.outputPath);
        assign_if_present(reports.value(), "includeValidationFindings", config.reports.includeValidationFindings);
        assign_if_present(reports.value(), "includeDependencyGraph", config.reports.includeDependencyGraph);
        assign_if_present(reports.value(), "format", report_format_from_string, config.reports.format);
    }
    config.reports.outputPath = expand_user_path(config.reports.outputPath).string();

    const sol::optional<sol::table> execution = root["execution"];
    if (execution.has_value()) {
        assign_if_present(execution.value(), "useTransactionDb", config.execution.useTransactionDb);
        assign_if_present(execution.value(), "deleteCommittedTransactions", config.execution.deleteCommittedTransactions);
        assign_if_present(execution.value(), "checkVirtualFileSystemWrite", config.execution.checkVirtualFileSystemWrite);
        assign_if_present(execution.value(), "stopOnFirstFailure", config.execution.stopOnFirstFailure);
        assign_if_present(execution.value(), "dryRun", config.execution.dryRun);
        assign_if_present(execution.value(), "transactionDatabasePath", config.execution.transactionDatabasePath);
    }
    config.execution.transactionDatabasePath = expand_user_path(config.execution.transactionDatabasePath).string();

    const sol::optional<sol::table> planner = root["planner"];
    if (planner.has_value()) {
        assign_if_present(planner.value(), "enableProxyExpansion", config.planner.enableProxyExpansion);
        assign_if_present(planner.value(), "autoDownloadMissingPlugins", config.planner.autoDownloadMissingPlugins);
        assign_if_present(planner.value(), "autoDownloadMissingDependencies", config.planner.autoDownloadMissingDependencies);
        assign_if_present(planner.value(), "buildDependencyDag", config.planner.buildDependencyDag);
        assign_if_present(planner.value(), "topologicallySortGraph", config.planner.topologicallySortGraph);

        const sol::optional<sol::table> aliases = planner.value()["systemAliases"];
        if (aliases.has_value()) {
            for (const auto& [key, value] : aliases.value()) {
                if (key.get_type() != sol::type::string || value.get_type() != sol::type::string) {
                    continue;
                }

                config.planner.systemAliases[to_lower(key.as<std::string>())] = to_lower(value.as<std::string>());
            }
        }
    }

    const sol::optional<sol::table> downloader = root["downloader"];
    if (downloader.has_value()) {
        assign_if_present(downloader.value(), "enabled", config.downloader.enabled);
        assign_if_present(downloader.value(), "followRedirects", config.downloader.followRedirects);
        assign_if_present(downloader.value(), "connectTimeoutSeconds", config.downloader.connectTimeoutSeconds);
        assign_if_present(downloader.value(), "requestTimeoutSeconds", config.downloader.requestTimeoutSeconds);
        assign_if_present(downloader.value(), "userAgent", config.downloader.userAgent);

        const sol::optional<sol::table> pluginSources = downloader.value()["pluginSources"];
        if (pluginSources.has_value()) {
            for (const auto& [key, value] : pluginSources.value()) {
                if (key.get_type() != sol::type::string || value.get_type() != sol::type::string) {
                    continue;
                }

                config.downloader.pluginSources[to_lower(key.as<std::string>())] = value.as<std::string>();
            }
        }
    }

    const sol::optional<sol::table> registry = root["registry"];
    if (registry.has_value()) {
        assign_if_present(registry.value(), "databasePath", config.registry.databasePath);
        assign_if_present(registry.value(), "remoteUrl", config.registry.remoteUrl);
        assign_if_present(registry.value(), "overlayPath", config.registry.overlayPath);
        assign_if_present(registry.value(), "pluginDirectory", config.registry.pluginDirectory);
        assign_if_present(registry.value(), "autoLoadPlugins", config.registry.autoLoadPlugins);
        assign_if_present(registry.value(), "shutDownPluginsOnExit", config.registry.shutDownPluginsOnExit);

        const sol::optional<sol::table> sources = registry.value()["sources"];
        if (sources.has_value()) {
            merge_registry_sources(config.registry.sources, load_registry_sources_from_table(sources.value()));
        }
    }
    config.registry.databasePath = expand_user_path(config.registry.databasePath).string();
    if (!config.registry.overlayPath.empty()) {
        config.registry.overlayPath = expand_user_path(config.registry.overlayPath).string();
    }
    config.registry.pluginDirectory = expand_user_path(config.registry.pluginDirectory).string();

    const sol::optional<sol::table> interaction = root["interaction"];
    if (interaction.has_value()) {
        assign_if_present(interaction.value(), "interactive", config.interaction.interactive);
        assign_if_present(interaction.value(), "promptBeforeUnsafeActions", config.interaction.promptBeforeUnsafeActions);
        assign_if_present(interaction.value(), "promptBeforeMissingPluginDownload", config.interaction.promptBeforeMissingPluginDownload);
        assign_if_present(interaction.value(), "promptBeforeMissingDependencyDownload", config.interaction.promptBeforeMissingDependencyDownload);
    }

    const sol::optional<sol::table> remote = root["remote"];
    if (remote.has_value()) {
        assign_if_present(remote.value(), "readonly", config.remote.readonly);
        assign_if_present(remote.value(), "maxConnections", config.remote.maxConnections);
    }

    const sol::optional<sol::table> sbom = root["sbom"];
    if (sbom.has_value()) {
        assign_if_present(sbom.value(), "defaultFormat", sbom_output_format_from_string, config.sbom.defaultFormat);
        assign_if_present(sbom.value(), "defaultOutputPath", config.sbom.defaultOutputPath);
        assign_if_present(sbom.value(), "prettyPrint", config.sbom.prettyPrint);
        assign_if_present(sbom.value(), "includeDependencyEdges", config.sbom.includeDependencyEdges);
    }
    if (!config.sbom.defaultOutputPath.empty()) {
        config.sbom.defaultOutputPath = expand_user_path(config.sbom.defaultOutputPath).string();
    }

    const sol::optional<sol::table> rqp = root["rqp"];
    if (rqp.has_value()) {
        config.rqp.repositories = load_string_array(rqp.value()["repositories"]);
        assign_if_present(rqp.value(), "statePath", config.rqp.statePath);
    }
    if (!config.rqp.statePath.empty()) {
        config.rqp.statePath = expand_user_path(config.rqp.statePath).string();
    }

    const sol::optional<sol::table> history = root["history"];
    if (history.has_value()) {
        assign_if_present(history.value(), "enabled",        config.history.enabled);
        assign_if_present(history.value(), "trackInstalled", config.history.trackInstalled);
        assign_if_present(history.value(), "historyPath",    config.history.historyPath);
        assign_if_present(history.value(), "maxLines",       config.history.maxLines);
        assign_if_present(history.value(), "maxSizeMb",      config.history.maxSizeMb);
    }
    if (!config.history.historyPath.empty()) {
        config.history.historyPath = expand_user_path(config.history.historyPath).string();
    }

    const sol::optional<sol::table> display = root["display"];
    if (display.has_value()) {
        assign_if_present(display.value(), "renderer", display_renderer_from_string, config.display.renderer);

        const sol::optional<sol::table> colors = display.value()["colors"];
        if (colors.has_value()) {
            assign_if_present(colors.value(), "rule",           config.display.colors.rule);
            assign_if_present(colors.value(), "header",         config.display.colors.header);
            assign_if_present(colors.value(), "summaryOk",      config.display.colors.summaryOk);
            assign_if_present(colors.value(), "summaryFail",    config.display.colors.summaryFail);
            assign_if_present(colors.value(), "barFill",        config.display.colors.barFill);
            assign_if_present(colors.value(), "barEmpty",       config.display.colors.barEmpty);
            assign_if_present(colors.value(), "barOuter",       config.display.colors.barOuter);
            assign_if_present(colors.value(), "step",           config.display.colors.step);
            assign_if_present(colors.value(), "successMarker",  config.display.colors.successMarker);
            assign_if_present(colors.value(), "failureMarker",  config.display.colors.failureMarker);
            assign_if_present(colors.value(), "message",        config.display.colors.message);
        }
    }

    return config;
}

ReqPackConfig apply_config_overrides(const ReqPackConfig& base, const ReqPackConfigOverrides& overrides) {
    ReqPackConfig config = base;

    if (overrides.logLevel.has_value()) config.logging.level = overrides.logLevel.value();
    if (overrides.logPattern.has_value()) config.logging.pattern = overrides.logPattern.value();
    if (overrides.fileOutput.has_value()) config.logging.fileOutput = overrides.fileOutput.value();
    if (overrides.logFilePath.has_value()) config.logging.filePath = expand_user_path(overrides.logFilePath.value()).string();
    if (overrides.enableBacktrace.has_value()) config.logging.enableBacktrace = overrides.enableBacktrace.value();
    if (overrides.backtraceSize.has_value()) config.logging.backtraceSize = overrides.backtraceSize.value();

    if (overrides.runSnykScan.has_value()) config.security.runSnykScan = overrides.runSnykScan.value();
    if (overrides.runOwaspScan.has_value()) config.security.runOwaspScan = overrides.runOwaspScan.value();
    if (overrides.severityThreshold.has_value()) config.security.severityThreshold = overrides.severityThreshold.value();
    if (overrides.scoreThreshold.has_value()) config.security.scoreThreshold = overrides.scoreThreshold.value();
    if (overrides.onUnsafe.has_value()) config.security.onUnsafe = overrides.onUnsafe.value();
    if (overrides.promptOnUnsafe.has_value()) config.security.promptOnUnsafe = overrides.promptOnUnsafe.value();
    if (overrides.osvDatabasePath.has_value()) config.security.osvDatabasePath = expand_user_path(overrides.osvDatabasePath.value()).string();
    if (overrides.osvFeedUrl.has_value()) config.security.osvFeedUrl = overrides.osvFeedUrl.value();
    if (overrides.osvRefreshMode.has_value()) config.security.osvRefreshMode = overrides.osvRefreshMode.value();
    if (overrides.osvRefreshIntervalSeconds.has_value()) config.security.osvRefreshIntervalSeconds = overrides.osvRefreshIntervalSeconds.value();
    if (overrides.osvOverlayPath.has_value()) config.security.osvOverlayPath = expand_user_path(overrides.osvOverlayPath.value()).string();
    if (overrides.onUnresolvedVersion.has_value()) config.security.onUnresolvedVersion = overrides.onUnresolvedVersion.value();
    if (overrides.strictEcosystemMapping.has_value()) config.security.strictEcosystemMapping = overrides.strictEcosystemMapping.value();
    if (overrides.includeWithdrawnInReport.has_value()) config.security.includeWithdrawnInReport = overrides.includeWithdrawnInReport.value();
    if (config.security.indexPath == expand_user_path(SecurityConfig{}.indexPath).string() &&
        config.security.osvDatabasePath != expand_user_path(SecurityConfig{}.osvDatabasePath).string()) {
        config.security.indexPath = config.security.osvDatabasePath;
    }
    if (!overrides.ignoreVulnerabilityIds.empty()) config.security.ignoreVulnerabilityIds = overrides.ignoreVulnerabilityIds;
    if (!overrides.allowVulnerabilityIds.empty()) config.security.allowVulnerabilityIds = overrides.allowVulnerabilityIds;

    if (overrides.reportEnabled.has_value()) config.reports.enabled = overrides.reportEnabled.value();
    if (overrides.reportFormat.has_value()) config.reports.format = overrides.reportFormat.value();
    if (overrides.reportOutputPath.has_value()) config.reports.outputPath = expand_user_path(overrides.reportOutputPath.value()).string();

    if (overrides.dryRun.has_value()) config.execution.dryRun = overrides.dryRun.value();
    if (overrides.stopOnFirstFailure.has_value()) config.execution.stopOnFirstFailure = overrides.stopOnFirstFailure.value();
    if (overrides.useTransactionDb.has_value()) config.execution.useTransactionDb = overrides.useTransactionDb.value();

    if (overrides.enableProxyExpansion.has_value()) config.planner.enableProxyExpansion = overrides.enableProxyExpansion.value();

    if (overrides.registryPath.has_value()) config.registry.databasePath = expand_user_path(overrides.registryPath.value()).string();
    if (overrides.pluginDirectory.has_value()) config.registry.pluginDirectory = expand_user_path(overrides.pluginDirectory.value()).string();
    if (overrides.autoLoadPlugins.has_value()) config.registry.autoLoadPlugins = overrides.autoLoadPlugins.value();

    if (overrides.interactive.has_value()) config.interaction.interactive = overrides.interactive.value();

    if (overrides.sbomDefaultFormat.has_value()) config.sbom.defaultFormat = overrides.sbomDefaultFormat.value();
    if (overrides.sbomDefaultOutputPath.has_value()) config.sbom.defaultOutputPath = expand_user_path(overrides.sbomDefaultOutputPath.value()).string();
    if (overrides.sbomPrettyPrint.has_value()) config.sbom.prettyPrint = overrides.sbomPrettyPrint.value();
    if (overrides.sbomIncludeDependencyEdges.has_value()) config.sbom.includeDependencyEdges = overrides.sbomIncludeDependencyEdges.value();

    return config;
}

bool consume_cli_config_flag(const std::vector<std::string>& arguments, std::size_t& index, ReqPackConfigOverrides& overrides) {
    const std::string& argument = arguments[index];

    auto starts_with = [&](const std::string& prefix) {
        return argument.rfind(prefix, 0) == 0;
    };

    auto inline_value = [&](const std::string& prefix) -> std::optional<std::string> {
        if (!starts_with(prefix)) {
            return std::nullopt;
        }

        const std::string value = argument.substr(prefix.size());
        if (value.empty()) {
            return std::nullopt;
        }

        return value;
    };

    auto require_value = [&](std::string& value) -> bool {
        if (index + 1 >= arguments.size()) {
            return false;
        }
        value = arguments[++index];
        return true;
    };

    std::string value;
    if (const std::optional<std::string> customConfig = inline_value("--config=")) {
        overrides.configPath = std::filesystem::path(customConfig.value());
        return true;
    }
    if (argument == "--config") {
        if (require_value(value)) overrides.configPath = std::filesystem::path(value);
        return true;
    }
    if (argument == "--log-level") {
        if (require_value(value)) overrides.logLevel = log_level_from_string(value);
        return true;
    }
    if (argument == "--log-pattern") {
        if (require_value(value)) overrides.logPattern = value;
        return true;
    }
    if (argument == "--log-file") {
        if (require_value(value)) {
            overrides.fileOutput = true;
            overrides.logFilePath = value;
        }
        return true;
    }
    if (argument == "--backtrace") {
        overrides.enableBacktrace = true;
        return true;
    }
    if (argument == "--dry-run") {
        overrides.dryRun = true;
        return true;
    }
    if (argument == "--snyk") {
        overrides.runSnykScan = true;
        return true;
    }
    if (argument == "--owasp") {
        overrides.runOwaspScan = true;
        return true;
    }
    if (argument == "--prompt-on-unsafe") {
        overrides.promptOnUnsafe = true;
        overrides.onUnsafe = UnsafeAction::PROMPT;
        return true;
    }
    if (argument == "--abort-on-unsafe") {
        overrides.onUnsafe = UnsafeAction::ABORT;
        return true;
    }
    if (argument == "--severity-threshold") {
        if (require_value(value)) overrides.severityThreshold = severity_level_from_string(value);
        return true;
    }
    if (argument == "--score-threshold") {
        if (require_value(value)) {
            try {
                overrides.scoreThreshold = std::stod(value);
            } catch (...) {
            }
        }
        return true;
    }
    if (argument == "--osv-db") {
        if (require_value(value)) overrides.osvDatabasePath = value;
        return true;
    }
    if (argument == "--osv-feed") {
        if (require_value(value)) overrides.osvFeedUrl = value;
        return true;
    }
    if (argument == "--osv-refresh") {
        if (require_value(value)) overrides.osvRefreshMode = osv_refresh_mode_from_string(value);
        return true;
    }
    if (argument == "--osv-refresh-interval") {
        if (require_value(value)) {
            try {
                overrides.osvRefreshIntervalSeconds = std::stol(value);
            } catch (...) {
            }
        }
        return true;
    }
    if (argument == "--osv-overlay") {
        if (require_value(value)) overrides.osvOverlayPath = value;
        return true;
    }
    if (argument == "--ignore-vuln") {
        if (require_value(value)) overrides.ignoreVulnerabilityIds.push_back(value);
        return true;
    }
    if (argument == "--allow-vuln") {
        if (require_value(value)) overrides.allowVulnerabilityIds.push_back(value);
        return true;
    }
    if (argument == "--fail-on-unresolved-version") {
        overrides.onUnresolvedVersion = UnsafeAction::ABORT;
        return true;
    }
    if (argument == "--prompt-on-unresolved-version") {
        overrides.onUnresolvedVersion = UnsafeAction::PROMPT;
        return true;
    }
    if (argument == "--strict-ecosystem-mapping") {
        overrides.strictEcosystemMapping = true;
        return true;
    }
    if (argument == "--include-withdrawn-in-report") {
        overrides.includeWithdrawnInReport = true;
        return true;
    }
    if (argument == "--report") {
        overrides.reportEnabled = true;
        return true;
    }
    if (argument == "--report-format") {
        if (require_value(value)) overrides.reportFormat = report_format_from_string(value);
        return true;
    }
    if (argument == "--report-output") {
        if (require_value(value)) overrides.reportOutputPath = value;
        return true;
    }
    if (argument == "--plugin-dir") {
        if (require_value(value)) overrides.pluginDirectory = value;
        return true;
    }
    if (argument == "--registry" || argument == "--registry-path") {
        if (require_value(value)) overrides.registryPath = value;
        return true;
    }
    if (const std::optional<std::string> registryPath = inline_value("--registry=")) {
        overrides.registryPath = registryPath.value();
        return true;
    }
    if (const std::optional<std::string> registryPath = inline_value("--registry-path=")) {
        overrides.registryPath = registryPath.value();
        return true;
    }
    if (argument == "--no-auto-load-plugins") {
        overrides.autoLoadPlugins = false;
        return true;
    }
    if (argument == "--no-proxy-expansion") {
        overrides.enableProxyExpansion = false;
        return true;
    }
    if (argument == "--stop-on-first-failure") {
        overrides.stopOnFirstFailure = true;
        return true;
    }
    if (argument == "--no-transaction-db") {
        overrides.useTransactionDb = false;
        return true;
    }
    if (argument == "--non-interactive") {
        overrides.interactive = false;
        return true;
    }
    if (argument == "--sbom-format") {
        if (require_value(value)) overrides.sbomDefaultFormat = sbom_output_format_from_string(value);
        return true;
    }
    if (argument == "--sbom-output") {
        if (require_value(value)) overrides.sbomDefaultOutputPath = value;
        return true;
    }
    if (argument == "--sbom-no-pretty") {
        overrides.sbomPrettyPrint = false;
        return true;
    }
    if (argument == "--sbom-no-dependency-edges") {
        overrides.sbomIncludeDependencyEdges = false;
        return true;
    }

    return false;
}

ReqPackConfigOverrides extract_cli_config_overrides(int argc, char* argv[]) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));

    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }

    return extract_cli_config_overrides(arguments);
}

ReqPackConfigOverrides extract_cli_config_overrides(const std::vector<std::string>& arguments) {
    ReqPackConfigOverrides overrides;

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        (void)consume_cli_config_flag(arguments, i, overrides);
    }

    return overrides;
}
