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

std::filesystem::path reqpack_home_directory() {
    return invoking_user_home_directory() / ".reqpack";
}

std::filesystem::path default_reqpack_config_path() {
    return reqpack_home_directory() / "config.lua";
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
        assign_if_present(security.value(), "runSnykScan", config.security.runSnykScan);
        assign_if_present(security.value(), "runOwaspScan", config.security.runOwaspScan);
        assign_if_present(security.value(), "scoreThreshold", config.security.scoreThreshold);
        assign_if_present(security.value(), "promptOnUnsafe", config.security.promptOnUnsafe);
        assign_if_present(security.value(), "allowUnassigned", config.security.allowUnassigned);
        assign_if_present(security.value(), "severityThreshold", severity_level_from_string, config.security.severityThreshold);
        assign_if_present(security.value(), "onUnsafe", unsafe_action_from_string, config.security.onUnsafe);
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
    }

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

    const sol::optional<sol::table> registry = root["registry"];
    if (registry.has_value()) {
        assign_if_present(registry.value(), "pluginDirectory", config.registry.pluginDirectory);
        assign_if_present(registry.value(), "autoLoadPlugins", config.registry.autoLoadPlugins);
        assign_if_present(registry.value(), "shutDownPluginsOnExit", config.registry.shutDownPluginsOnExit);
    }
    config.registry.pluginDirectory = expand_user_path(config.registry.pluginDirectory).string();

    const sol::optional<sol::table> interaction = root["interaction"];
    if (interaction.has_value()) {
        assign_if_present(interaction.value(), "interactive", config.interaction.interactive);
        assign_if_present(interaction.value(), "promptBeforeUnsafeActions", config.interaction.promptBeforeUnsafeActions);
        assign_if_present(interaction.value(), "promptBeforeMissingPluginDownload", config.interaction.promptBeforeMissingPluginDownload);
        assign_if_present(interaction.value(), "promptBeforeMissingDependencyDownload", config.interaction.promptBeforeMissingDependencyDownload);
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

    if (overrides.reportEnabled.has_value()) config.reports.enabled = overrides.reportEnabled.value();
    if (overrides.reportFormat.has_value()) config.reports.format = overrides.reportFormat.value();
    if (overrides.reportOutputPath.has_value()) config.reports.outputPath = expand_user_path(overrides.reportOutputPath.value()).string();

    if (overrides.dryRun.has_value()) config.execution.dryRun = overrides.dryRun.value();
    if (overrides.stopOnFirstFailure.has_value()) config.execution.stopOnFirstFailure = overrides.stopOnFirstFailure.value();
    if (overrides.useTransactionDb.has_value()) config.execution.useTransactionDb = overrides.useTransactionDb.value();

    if (overrides.enableProxyExpansion.has_value()) config.planner.enableProxyExpansion = overrides.enableProxyExpansion.value();

    if (overrides.pluginDirectory.has_value()) config.registry.pluginDirectory = expand_user_path(overrides.pluginDirectory.value()).string();
    if (overrides.autoLoadPlugins.has_value()) config.registry.autoLoadPlugins = overrides.autoLoadPlugins.value();

    if (overrides.interactive.has_value()) config.interaction.interactive = overrides.interactive.value();

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

    return false;
}

ReqPackConfigOverrides extract_cli_config_overrides(int argc, char* argv[]) {
    ReqPackConfigOverrides overrides;
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));

    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        (void)consume_cli_config_flag(arguments, i, overrides);
    }

    return overrides;
}
