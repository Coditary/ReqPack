#include "configuration_internal.h"

ReqPackConfig apply_config_overrides(const ReqPackConfig& base, const ReqPackConfigOverrides& overrides) {
    ReqPackConfig config = base;

    if (overrides.consoleOutput.has_value()) config.logging.consoleOutput = overrides.consoleOutput.value();
    if (overrides.logLevel.has_value()) config.logging.level = overrides.logLevel.value();
    if (overrides.logPattern.has_value()) config.logging.pattern = overrides.logPattern.value();
    if (overrides.fileOutput.has_value()) config.logging.fileOutput = overrides.fileOutput.value();
    if (overrides.logFilePath.has_value()) config.logging.filePath = configuration_internal::expand_user_path(overrides.logFilePath.value()).string();
    if (overrides.structuredFileOutput.has_value()) config.logging.structuredFileOutput = overrides.structuredFileOutput.value();
    if (overrides.structuredLogFilePath.has_value()) config.logging.structuredFilePath = configuration_internal::expand_user_path(overrides.structuredLogFilePath.value()).string();
    if (overrides.captureDisplayEvents.has_value()) config.logging.captureDisplayEvents = overrides.captureDisplayEvents.value();
    if (!overrides.enabledLogCategories.empty()) config.logging.enabledCategories = configuration_internal::normalize_string_list(overrides.enabledLogCategories);
    if (overrides.enableBacktrace.has_value()) config.logging.enableBacktrace = overrides.enableBacktrace.value();
    if (overrides.backtraceSize.has_value()) config.logging.backtraceSize = overrides.backtraceSize.value();

    if (overrides.severityThreshold.has_value()) config.security.severityThreshold = overrides.severityThreshold.value();
    if (overrides.scoreThreshold.has_value()) config.security.scoreThreshold = overrides.scoreThreshold.value();
    if (overrides.onUnsafe.has_value()) config.security.onUnsafe = overrides.onUnsafe.value();
    if (overrides.promptOnUnsafe.has_value()) config.security.promptOnUnsafe = overrides.promptOnUnsafe.value();
    if (overrides.osvDatabasePath.has_value()) config.security.osvDatabasePath = configuration_internal::expand_user_path(overrides.osvDatabasePath.value()).string();
    if (overrides.osvFeedUrl.has_value()) config.security.osvFeedUrl = overrides.osvFeedUrl.value();
    if (overrides.osvRefreshMode.has_value()) config.security.osvRefreshMode = overrides.osvRefreshMode.value();
    if (overrides.osvRefreshIntervalSeconds.has_value()) config.security.osvRefreshIntervalSeconds = overrides.osvRefreshIntervalSeconds.value();
    if (overrides.osvOverlayPath.has_value()) config.security.osvOverlayPath = configuration_internal::expand_user_path(overrides.osvOverlayPath.value()).string();
    if (overrides.onUnresolvedVersion.has_value()) config.security.onUnresolvedVersion = overrides.onUnresolvedVersion.value();
    if (overrides.strictEcosystemMapping.has_value()) config.security.strictEcosystemMapping = overrides.strictEcosystemMapping.value();
    if (overrides.includeWithdrawnInReport.has_value()) config.security.includeWithdrawnInReport = overrides.includeWithdrawnInReport.value();
    if (config.security.indexPath == default_reqpack_security_index_path().string() &&
        config.security.osvDatabasePath != default_reqpack_osv_database_path().string()) {
        config.security.indexPath = config.security.osvDatabasePath;
    }
    if (!overrides.ignoreVulnerabilityIds.empty()) config.security.ignoreVulnerabilityIds = overrides.ignoreVulnerabilityIds;
    if (!overrides.allowVulnerabilityIds.empty()) config.security.allowVulnerabilityIds = overrides.allowVulnerabilityIds;

    if (overrides.reportEnabled.has_value()) config.reports.enabled = overrides.reportEnabled.value();
    if (overrides.reportFormat.has_value()) config.reports.format = overrides.reportFormat.value();
    if (overrides.reportOutputPath.has_value()) config.reports.outputPath = configuration_internal::expand_user_path(overrides.reportOutputPath.value()).string();

    if (overrides.dryRun.has_value()) config.execution.dryRun = overrides.dryRun.value();
    if (overrides.stopOnFirstFailure.has_value()) config.execution.stopOnFirstFailure = overrides.stopOnFirstFailure.value();
    if (overrides.useTransactionDb.has_value()) config.execution.useTransactionDb = overrides.useTransactionDb.value();
    if (overrides.jobs.has_value()) config.execution.jobs = std::max(1u, overrides.jobs.value());
    if (overrides.jobsMode.has_value()) config.execution.jobsMode = overrides.jobsMode.value();

    if (overrides.enableProxyExpansion.has_value()) config.planner.enableProxyExpansion = overrides.enableProxyExpansion.value();
    for (const auto& [name, target] : overrides.proxyDefaultTargets) {
        config.planner.proxies[name].defaultTarget = configuration_internal::to_lower_copy(target);
    }

    if (overrides.registryPath.has_value()) config.registry.databasePath = configuration_internal::expand_user_path(overrides.registryPath.value()).string();
    if (overrides.pluginDirectory.has_value()) config.registry.pluginDirectory = configuration_internal::expand_user_path(overrides.pluginDirectory.value()).string();
    if (overrides.autoLoadPlugins.has_value()) config.registry.autoLoadPlugins = overrides.autoLoadPlugins.value();

    if (overrides.interactive.has_value()) config.interaction.interactive = overrides.interactive.value();
    if (overrides.archivePassword.has_value()) config.archives.password = overrides.archivePassword.value();

    if (overrides.sbomDefaultFormat.has_value()) config.sbom.defaultFormat = overrides.sbomDefaultFormat.value();
    if (overrides.sbomDefaultOutputPath.has_value()) config.sbom.defaultOutputPath = configuration_internal::expand_user_path(overrides.sbomDefaultOutputPath.value()).string();
    if (overrides.sbomPrettyPrint.has_value()) config.sbom.prettyPrint = overrides.sbomPrettyPrint.value();
    if (overrides.sbomIncludeDependencyEdges.has_value()) config.sbom.includeDependencyEdges = overrides.sbomIncludeDependencyEdges.value();
    if (overrides.sbomSkipMissingPackages.has_value()) config.sbom.skipMissingPackages = overrides.sbomSkipMissingPackages.value();

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

    auto parse_define = [&](const std::string& raw) -> bool {
        if (!starts_with("-D") || raw.size() <= 2) {
            return false;
        }

        const std::size_t separator = raw.find('=', 2);
        if (separator == std::string::npos || separator == 2 || separator + 1 >= raw.size()) {
            return false;
        }

        const std::string key = configuration_internal::to_lower_copy(raw.substr(2, separator - 2));
        const std::string value = configuration_internal::to_lower_copy(raw.substr(separator + 1));
        const std::string prefix = "proxy.";
        const std::string suffix = ".default";
        if (key.rfind(prefix, 0) != 0 || key.size() <= prefix.size() + suffix.size() ||
            key.substr(key.size() - suffix.size()) != suffix) {
            return false;
        }

        const std::string proxyName = key.substr(prefix.size(), key.size() - prefix.size() - suffix.size());
        if (proxyName.empty() || value.empty()) {
            return false;
        }

        overrides.proxyDefaultTargets[proxyName] = value;
        return true;
    };

    auto require_value = [&](std::string& value) -> bool {
        if (index + 1 >= arguments.size()) {
            return false;
        }
        value = arguments[++index];
        return true;
    };

    auto set_error = [&](const std::string& message) {
        overrides.errorMessage = message;
    };

    std::string value;
    if (starts_with("-D")) {
        (void)parse_define(argument);
        return true;
    }
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
    if (argument == "--log-console") {
        overrides.consoleOutput = true;
        return true;
    }
    if (argument == "--no-log-console") {
        overrides.consoleOutput = false;
        return true;
    }
    if (argument == "--verbose" || argument == "-v") {
        overrides.consoleOutput = true;
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
    if (argument == "--structured-log-file") {
        if (require_value(value)) {
            overrides.structuredFileOutput = true;
            overrides.structuredLogFilePath = value;
        }
        return true;
    }
    if (argument == "--log-capture-display") {
        overrides.captureDisplayEvents = true;
        return true;
    }
    if (argument == "--no-log-capture-display") {
        overrides.captureDisplayEvents = false;
        return true;
    }
    if (argument == "--log-category") {
        if (!require_value(value)) {
            set_error("missing value for --log-category");
            return true;
        }
        overrides.enabledLogCategories.push_back(configuration_internal::to_lower_copy(value));
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
    if (argument == "--jobs") {
        if (!require_value(value)) {
            set_error("missing value for --jobs");
            return true;
        }
        if (overrides.jobsMode == ExecutionJobsMode::MAX) {
            set_error("cannot combine --jobs with --jobs-max");
            return true;
        }
        const std::optional<unsigned int> parsedJobs = configuration_internal::unsigned_int_from_string(value);
        if (!parsedJobs.has_value()) {
            set_error("invalid value for --jobs: " + value);
            return true;
        }
        overrides.jobs = parsedJobs.value();
        overrides.jobsMode = ExecutionJobsMode::FIXED;
        return true;
    }
    if (argument == "--jobs-max") {
        if (overrides.jobs.has_value()) {
            set_error("cannot combine --jobs with --jobs-max");
            return true;
        }
        overrides.jobsMode = ExecutionJobsMode::MAX;
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
    if (argument == "--archive-password") {
        if (require_value(value)) overrides.archivePassword = value;
        return true;
    }
    if (const std::optional<std::string> archivePassword = inline_value("--archive-password=")) {
        overrides.archivePassword = archivePassword.value();
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
    if (argument == "--sbom-skip-missing-packages") {
        overrides.sbomSkipMissingPackages = true;
        return true;
    }
    if (argument == "--sbom-fail-on-missing-package") {
        overrides.sbomSkipMissingPackages = false;
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
        if (overrides.errorMessage.has_value()) {
            break;
        }
    }

    return overrides;
}
