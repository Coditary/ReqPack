#include "configuration_internal.h"

#include <sol/sol.hpp>

#include <thread>

namespace {

void load_logging_section(const sol::table& root, ReqPackConfig& config) {
    const sol::optional<sol::table> logging = root["logging"];
    if (logging.has_value()) {
        configuration_internal::assign_if_present(logging.value(), "consoleOutput", config.logging.consoleOutput);
        configuration_internal::assign_if_present(logging.value(), "fileOutput", config.logging.fileOutput);
        configuration_internal::assign_if_present(logging.value(), "filePath", config.logging.filePath);
        configuration_internal::assign_if_present(logging.value(), "structuredFileOutput", config.logging.structuredFileOutput);
        configuration_internal::assign_if_present(logging.value(), "structuredFilePath", config.logging.structuredFilePath);
        configuration_internal::assign_if_present(logging.value(), "captureDisplayEvents", config.logging.captureDisplayEvents);
        configuration_internal::assign_if_present(logging.value(), "enableBacktrace", config.logging.enableBacktrace);
        configuration_internal::assign_if_present(logging.value(), "backtraceSize", config.logging.backtraceSize);
        configuration_internal::assign_if_present(logging.value(), "pattern", config.logging.pattern);
        configuration_internal::assign_if_present(logging.value(), "level", log_level_from_string, config.logging.level);
        const std::vector<std::string> categories = configuration_internal::load_string_array(logging.value()["enabledCategories"]);
        if (!categories.empty()) {
            config.logging.enabledCategories = configuration_internal::normalize_string_list(categories);
        }
    }
    config.logging.filePath = configuration_internal::expand_user_path(config.logging.filePath).string();
    config.logging.structuredFilePath = configuration_internal::expand_user_path(config.logging.structuredFilePath).string();
}

void load_security_section(const sol::table& root, ReqPackConfig& config) {
    const sol::optional<sol::table> security = root["security"];
    if (security.has_value()) {
        configuration_internal::assign_if_present(security.value(), "enabled", config.security.enabled);
        configuration_internal::assign_if_present(security.value(), "autoFetch", config.security.autoFetch);
        configuration_internal::assign_if_present(security.value(), "requireThinLayer", config.security.requireThinLayer);
        configuration_internal::assign_if_present(security.value(), "scoreThreshold", config.security.scoreThreshold);
        configuration_internal::assign_if_present(security.value(), "promptOnUnsafe", config.security.promptOnUnsafe);
        configuration_internal::assign_if_present(security.value(), "allowUnassigned", config.security.allowUnassigned);
        configuration_internal::assign_if_present(security.value(), "defaultGateway", config.security.defaultGateway);
        configuration_internal::assign_if_present(security.value(), "cachePath", config.security.cachePath);
        configuration_internal::assign_if_present(security.value(), "indexPath", config.security.indexPath);
        configuration_internal::assign_if_present(security.value(), "severityThreshold", severity_level_from_string, config.security.severityThreshold);
        configuration_internal::assign_if_present(security.value(), "onUnsafe", unsafe_action_from_string, config.security.onUnsafe);
        configuration_internal::assign_if_present(security.value(), "osvDatabasePath", config.security.osvDatabasePath);
        configuration_internal::assign_if_present(security.value(), "osvFeedUrl", config.security.osvFeedUrl);
        configuration_internal::assign_if_present(security.value(), "osvRefreshMode", osv_refresh_mode_from_string, config.security.osvRefreshMode);
        configuration_internal::assign_if_present(security.value(), "osvRefreshIntervalSeconds", config.security.osvRefreshIntervalSeconds);
        configuration_internal::assign_if_present(security.value(), "osvOverlayPath", config.security.osvOverlayPath);
        configuration_internal::assign_if_present(security.value(), "onUnresolvedVersion", unsafe_action_from_string, config.security.onUnresolvedVersion);
        configuration_internal::assign_if_present(security.value(), "strictEcosystemMapping", config.security.strictEcosystemMapping);
        configuration_internal::assign_if_present(security.value(), "includeWithdrawnInReport", config.security.includeWithdrawnInReport);

        const std::vector<std::string> ignoreIds = configuration_internal::load_string_array(security.value()["ignoreVulnerabilityIds"]);
        if (!ignoreIds.empty()) {
            config.security.ignoreVulnerabilityIds = ignoreIds;
        }

        const std::vector<std::string> allowIds = configuration_internal::load_string_array(security.value()["allowVulnerabilityIds"]);
        if (!allowIds.empty()) {
            config.security.allowVulnerabilityIds = allowIds;
        }

        const std::map<std::string, std::string> ecosystemMap = configuration_internal::load_string_map(security.value()["osvEcosystemMap"]);
        for (const auto& [system, ecosystem] : ecosystemMap) {
            config.security.osvEcosystemMap[system] = ecosystem;
        }

        const std::map<std::string, std::string> securityEcosystemMap = configuration_internal::load_string_map(security.value()["ecosystemMap"]);
        for (const auto& [system, ecosystem] : securityEcosystemMap) {
            config.security.ecosystemMap[system] = ecosystem;
        }

        const std::map<std::string, SecurityGatewayConfig> gateways = configuration_internal::load_security_gateway_map(security.value()["gateways"]);
        for (const auto& [name, gateway] : gateways) {
            config.security.gateways[name] = gateway;
        }

        const std::map<std::string, SecurityBackendConfig> backends = configuration_internal::load_security_backend_map(security.value()["backends"]);
        for (const auto& [name, backend] : backends) {
            config.security.backends[name] = backend;
        }
    }

    const std::string defaultIndexPath = default_reqpack_security_index_path().string();
    const std::string defaultOsvDatabasePath = default_reqpack_osv_database_path().string();
    if (config.security.indexPath == defaultIndexPath && config.security.osvDatabasePath != defaultOsvDatabasePath) {
        config.security.indexPath = config.security.osvDatabasePath;
    }
    config.security.defaultGateway = configuration_internal::to_lower_copy(config.security.defaultGateway);
    config.security.cachePath = configuration_internal::expand_user_path(config.security.cachePath).string();
    config.security.indexPath = configuration_internal::expand_user_path(config.security.indexPath).string();
    config.security.osvDatabasePath = configuration_internal::expand_user_path(config.security.osvDatabasePath).string();
    if (!config.security.osvOverlayPath.empty()) {
        config.security.osvOverlayPath = configuration_internal::expand_user_path(config.security.osvOverlayPath).string();
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
}

void load_reports_section(const sol::table& root, ReqPackConfig& config) {
    const sol::optional<sol::table> reports = root["reports"];
    if (reports.has_value()) {
        configuration_internal::assign_if_present(reports.value(), "enabled", config.reports.enabled);
        configuration_internal::assign_if_present(reports.value(), "outputPath", config.reports.outputPath);
        configuration_internal::assign_if_present(reports.value(), "includeValidationFindings", config.reports.includeValidationFindings);
        configuration_internal::assign_if_present(reports.value(), "includeDependencyGraph", config.reports.includeDependencyGraph);
        configuration_internal::assign_if_present(reports.value(), "format", report_format_from_string, config.reports.format);
    }
    config.reports.outputPath = configuration_internal::expand_user_path(config.reports.outputPath).string();
}

void load_execution_section(const sol::table& root, ReqPackConfig& config) {
    const sol::optional<sol::table> execution = root["execution"];
    if (execution.has_value()) {
        configuration_internal::assign_if_present(execution.value(), "useTransactionDb", config.execution.useTransactionDb);
        configuration_internal::assign_if_present(execution.value(), "deleteCommittedTransactions", config.execution.deleteCommittedTransactions);
        configuration_internal::assign_if_present(execution.value(), "checkVirtualFileSystemWrite", config.execution.checkVirtualFileSystemWrite);
        configuration_internal::assign_if_present(execution.value(), "stopOnFirstFailure", config.execution.stopOnFirstFailure);
        configuration_internal::assign_if_present(execution.value(), "dryRun", config.execution.dryRun);
        if (const sol::optional<int> jobs = execution.value()["jobs"]; jobs.has_value() && jobs.value() > 0) {
            config.execution.jobs = static_cast<unsigned int>(jobs.value());
        }
        configuration_internal::assign_if_present(execution.value(), "jobsMode", execution_jobs_mode_from_string, config.execution.jobsMode);
        configuration_internal::assign_if_present(execution.value(), "transactionDatabasePath", config.execution.transactionDatabasePath);
    }
    config.execution.transactionDatabasePath = configuration_internal::expand_user_path(config.execution.transactionDatabasePath).string();
}

void load_planner_section(const sol::table& root, ReqPackConfig& config) {
    const sol::optional<sol::table> planner = root["planner"];
    if (planner.has_value()) {
        configuration_internal::assign_if_present(planner.value(), "enableProxyExpansion", config.planner.enableProxyExpansion);
        configuration_internal::assign_if_present(planner.value(), "autoDownloadMissingPlugins", config.planner.autoDownloadMissingPlugins);
        configuration_internal::assign_if_present(planner.value(), "autoDownloadMissingDependencies", config.planner.autoDownloadMissingDependencies);
        configuration_internal::assign_if_present(planner.value(), "buildDependencyDag", config.planner.buildDependencyDag);
        configuration_internal::assign_if_present(planner.value(), "topologicallySortGraph", config.planner.topologicallySortGraph);

        const sol::optional<sol::table> aliases = planner.value()["systemAliases"];
        if (aliases.has_value()) {
            for (const auto& [key, value] : aliases.value()) {
                if (key.get_type() != sol::type::string || value.get_type() != sol::type::string) {
                    continue;
                }

                config.planner.systemAliases[configuration_internal::to_lower_copy(key.as<std::string>())] =
                    configuration_internal::to_lower_copy(value.as<std::string>());
            }
        }

        const std::map<std::string, ProxyConfig> proxies = configuration_internal::load_proxy_config_map(planner.value()["proxies"]);
        for (const auto& [name, proxy] : proxies) {
            config.planner.proxies[name] = proxy;
        }
    }
}

void load_downloader_section(const sol::table& root, ReqPackConfig& config) {
    const sol::optional<sol::table> downloader = root["downloader"];
    if (downloader.has_value()) {
        configuration_internal::assign_if_present(downloader.value(), "enabled", config.downloader.enabled);
        configuration_internal::assign_if_present(downloader.value(), "followRedirects", config.downloader.followRedirects);
        configuration_internal::assign_if_present(downloader.value(), "connectTimeoutSeconds", config.downloader.connectTimeoutSeconds);
        configuration_internal::assign_if_present(downloader.value(), "requestTimeoutSeconds", config.downloader.requestTimeoutSeconds);
        configuration_internal::assign_if_present(downloader.value(), "userAgent", config.downloader.userAgent);

        const sol::optional<sol::table> pluginSources = downloader.value()["pluginSources"];
        if (pluginSources.has_value()) {
            for (const auto& [key, value] : pluginSources.value()) {
                if (key.get_type() != sol::type::string || value.get_type() != sol::type::string) {
                    continue;
                }

                config.downloader.pluginSources[configuration_internal::to_lower_copy(key.as<std::string>())] = value.as<std::string>();
            }
        }
    }
}

void load_registry_section(const sol::table& root, ReqPackConfig& config) {
    const sol::optional<sol::table> registry = root["registry"];
    if (registry.has_value()) {
        configuration_internal::assign_if_present(registry.value(), "databasePath", config.registry.databasePath);
        configuration_internal::assign_if_present(registry.value(), "remoteUrl", config.registry.remoteUrl);
        configuration_internal::assign_if_present(registry.value(), "remoteBranch", config.registry.remoteBranch);
        configuration_internal::assign_if_present(registry.value(), "remotePluginsPath", config.registry.remotePluginsPath);
        configuration_internal::assign_if_present(registry.value(), "overlayPath", config.registry.overlayPath);
        configuration_internal::assign_if_present(registry.value(), "pluginDirectory", config.registry.pluginDirectory);
        configuration_internal::assign_if_present(registry.value(), "autoLoadPlugins", config.registry.autoLoadPlugins);
        configuration_internal::assign_if_present(registry.value(), "shutDownPluginsOnExit", config.registry.shutDownPluginsOnExit);

        const sol::optional<sol::table> sources = registry.value()["sources"];
        if (sources.has_value()) {
            configuration_internal::merge_registry_sources(config.registry.sources, configuration_internal::load_registry_sources_from_table(sources.value()));
        }
    }
    config.registry.databasePath = configuration_internal::expand_user_path(config.registry.databasePath).string();
    if (!config.registry.overlayPath.empty()) {
        config.registry.overlayPath = configuration_internal::expand_user_path(config.registry.overlayPath).string();
    }
    config.registry.pluginDirectory = configuration_internal::expand_user_path(config.registry.pluginDirectory).string();
}

void load_archives_section(const sol::table& root, ReqPackConfig& config) {
    const sol::optional<sol::table> archives = root["archives"];
    if (archives.has_value()) {
        if (const sol::optional<std::string> password = archives.value()["password"]; password.has_value()) {
            config.archives.password = configuration_internal::expand_env_reference(password.value());
        }
    }
}

void load_interaction_section(const sol::table& root, ReqPackConfig& config) {
    const sol::optional<sol::table> interaction = root["interaction"];
    if (interaction.has_value()) {
        configuration_internal::assign_if_present(interaction.value(), "interactive", config.interaction.interactive);
        configuration_internal::assign_if_present(interaction.value(), "promptBeforeUnsafeActions", config.interaction.promptBeforeUnsafeActions);
        configuration_internal::assign_if_present(interaction.value(), "promptBeforeMissingPluginDownload", config.interaction.promptBeforeMissingPluginDownload);
        configuration_internal::assign_if_present(interaction.value(), "promptBeforeMissingDependencyDownload", config.interaction.promptBeforeMissingDependencyDownload);
    }
}

void load_remote_section(const sol::table& root, ReqPackConfig& config) {
    const sol::optional<sol::table> remote = root["remote"];
    if (remote.has_value()) {
        configuration_internal::assign_if_present(remote.value(), "readonly", config.remote.readonly);
        configuration_internal::assign_if_present(remote.value(), "maxConnections", config.remote.maxConnections);
    }
}

void load_sbom_section(const sol::table& root, ReqPackConfig& config) {
    const sol::optional<sol::table> sbom = root["sbom"];
    if (sbom.has_value()) {
        configuration_internal::assign_if_present(sbom.value(), "defaultFormat", sbom_output_format_from_string, config.sbom.defaultFormat);
        configuration_internal::assign_if_present(sbom.value(), "defaultOutputPath", config.sbom.defaultOutputPath);
        configuration_internal::assign_if_present(sbom.value(), "prettyPrint", config.sbom.prettyPrint);
        configuration_internal::assign_if_present(sbom.value(), "includeDependencyEdges", config.sbom.includeDependencyEdges);
        configuration_internal::assign_if_present(sbom.value(), "skipMissingPackages", config.sbom.skipMissingPackages);
    }
    if (!config.sbom.defaultOutputPath.empty()) {
        config.sbom.defaultOutputPath = configuration_internal::expand_user_path(config.sbom.defaultOutputPath).string();
    }
}

bool load_rqp_section(const sol::table& root, ReqPackConfig& config, const ReqPackConfig& fallback) {
    const sol::optional<sol::table> rqp = root["rqp"];
    if (rqp.has_value()) {
        config.rqp.repositories = configuration_internal::load_string_array(rqp.value()["repositories"]);
        configuration_internal::assign_if_present(rqp.value(), "statePath", config.rqp.statePath);
        bool aliasesOk = true;
        const auto aliases = configuration_internal::load_string_list_map(rqp.value()["systemAliases"], aliasesOk);
        if (!aliasesOk) {
            config = fallback;
            return false;
        }
        for (const auto& [name, members] : aliases) {
            config.rqp.systemAliases[name] = members;
        }
    }
    if (!config.rqp.statePath.empty()) {
        config.rqp.statePath = configuration_internal::expand_user_path(config.rqp.statePath).string();
    }
    return true;
}

void load_self_update_section(const sol::table& root, ReqPackConfig& config) {
    const sol::optional<sol::table> selfUpdate = root["selfUpdate"];
    if (selfUpdate.has_value()) {
        configuration_internal::assign_if_present(selfUpdate.value(), "repoUrl", config.selfUpdate.repoUrl);
        configuration_internal::assign_if_present(selfUpdate.value(), "releaseApiBaseUrl", config.selfUpdate.releaseApiBaseUrl);
        configuration_internal::assign_if_present(selfUpdate.value(), "releaseTag", config.selfUpdate.releaseTag);
        configuration_internal::assign_if_present(selfUpdate.value(), "binaryDirectory", config.selfUpdate.binaryDirectory);
        configuration_internal::assign_if_present(selfUpdate.value(), "linkPath", config.selfUpdate.linkPath);
    }
    if (!config.selfUpdate.binaryDirectory.empty()) {
        config.selfUpdate.binaryDirectory = configuration_internal::expand_user_path(config.selfUpdate.binaryDirectory).string();
    }
    if (!config.selfUpdate.linkPath.empty()) {
        config.selfUpdate.linkPath = configuration_internal::expand_user_path(config.selfUpdate.linkPath).string();
    }
}

void load_repositories_section(const sol::table& root, ReqPackConfig& config) {
    const sol::optional<sol::table> repositories = root["repositories"];
    if (repositories.has_value()) {
        const auto parsedRepositories = configuration_internal::load_repository_map(repositories.value());
        for (const auto& [ecosystem, entries] : parsedRepositories) {
            config.repositories[ecosystem] = entries;
        }
    }
}

void load_history_section(const sol::table& root, ReqPackConfig& config) {
    const sol::optional<sol::table> history = root["history"];
    if (history.has_value()) {
        configuration_internal::assign_if_present(history.value(), "enabled", config.history.enabled);
        configuration_internal::assign_if_present(history.value(), "trackInstalled", config.history.trackInstalled);
        configuration_internal::assign_if_present(history.value(), "historyPath", config.history.historyPath);
        configuration_internal::assign_if_present(history.value(), "maxLines", config.history.maxLines);
        configuration_internal::assign_if_present(history.value(), "maxSizeMb", config.history.maxSizeMb);
    }
    if (!config.history.historyPath.empty()) {
        config.history.historyPath = configuration_internal::expand_user_path(config.history.historyPath).string();
    }
}

void load_display_section(const sol::table& root, ReqPackConfig& config) {
    const sol::optional<sol::table> display = root["display"];
    if (display.has_value()) {
        configuration_internal::assign_if_present(display.value(), "renderer", display_renderer_from_string, config.display.renderer);

        const sol::optional<sol::table> colors = display.value()["colors"];
        if (colors.has_value()) {
            configuration_internal::assign_if_present(colors.value(), "rule", config.display.colors.rule);
            configuration_internal::assign_if_present(colors.value(), "header", config.display.colors.header);
            configuration_internal::assign_if_present(colors.value(), "summaryOk", config.display.colors.summaryOk);
            configuration_internal::assign_if_present(colors.value(), "summaryFail", config.display.colors.summaryFail);
            configuration_internal::assign_if_present(colors.value(), "barFill", config.display.colors.barFill);
            configuration_internal::assign_if_present(colors.value(), "barEmpty", config.display.colors.barEmpty);
            configuration_internal::assign_if_present(colors.value(), "barOuter", config.display.colors.barOuter);
            configuration_internal::assign_if_present(colors.value(), "step", config.display.colors.step);
            configuration_internal::assign_if_present(colors.value(), "successMarker", config.display.colors.successMarker);
            configuration_internal::assign_if_present(colors.value(), "failureMarker", config.display.colors.failureMarker);
            configuration_internal::assign_if_present(colors.value(), "message", config.display.colors.message);
        }
    }
}

}  // namespace

ReqPackConfig load_config_from_lua(const std::filesystem::path& configPath, const ReqPackConfig& fallback) {
    ReqPackConfig config = fallback;
    const std::filesystem::path resolvedConfigPath = configuration_internal::expand_user_path(configPath);
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

    configuration_internal::assign_if_present(root, "applicationName", config.applicationName);
    configuration_internal::assign_if_present(root, "version", config.version);

    load_logging_section(root, config);
    load_security_section(root, config);
    load_reports_section(root, config);
    load_execution_section(root, config);
    load_planner_section(root, config);
    load_downloader_section(root, config);
    load_registry_section(root, config);
    load_archives_section(root, config);
    load_interaction_section(root, config);
    load_remote_section(root, config);
    load_sbom_section(root, config);
    if (!load_rqp_section(root, config, fallback)) {
        return fallback;
    }
    load_self_update_section(root, config);
    load_repositories_section(root, config);
    load_history_section(root, config);
    load_display_section(root, config);

    return config;
}

std::vector<RepositoryEntry> repositories_for_ecosystem(const ReqPackConfig& config, const std::string& ecosystem) {
    const auto it = config.repositories.find(configuration_internal::to_lower_copy(ecosystem));
    if (it == config.repositories.end()) {
        return {};
    }

    std::vector<RepositoryEntry> repositories = it->second;
    std::stable_sort(repositories.begin(), repositories.end(), [](const RepositoryEntry& left, const RepositoryEntry& right) {
        return left.priority < right.priority;
    });
    return repositories;
}

std::optional<ProxyConfig> proxy_config_for_system(const ReqPackConfig& config, const std::string& system) {
    const auto it = config.planner.proxies.find(configuration_internal::to_lower_copy(system));
    if (it == config.planner.proxies.end()) {
        return std::nullopt;
    }
    return it->second;
}
