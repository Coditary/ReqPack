#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

enum class SeverityLevel {
    UNASSIGNED,
    LOW,
    MEDIUM,
    HIGH,
    CRITICAL
};

enum class LogLevel {
    TRACE,
    DEBUG,
    INFO,
    WARN,
    ERROR,
    CRITICAL
};

enum class ReportFormat {
    NONE,
    JSON,
    CYCLONEDX
};

enum class UnsafeAction {
    CONTINUE,
    PROMPT,
    ABORT
};

struct LoggingConfig {
    LogLevel level{LogLevel::INFO};
    bool consoleOutput{true};
    bool fileOutput{false};
    std::string filePath{"reqpack.log"};
    bool enableBacktrace{false};
    std::size_t backtraceSize{32};
    std::string pattern{"[%^%l%$] %v"};
};

struct SecurityConfig {
    bool enabled{true};
    bool runSnykScan{false};
    bool runOwaspScan{false};
    SeverityLevel severityThreshold{SeverityLevel::CRITICAL};
    double scoreThreshold{0.0};
    UnsafeAction onUnsafe{UnsafeAction::CONTINUE};
    bool promptOnUnsafe{false};
    bool allowUnassigned{true};
};

struct ReportConfig {
    bool enabled{false};
    ReportFormat format{ReportFormat::NONE};
    std::string outputPath{"reqpack-report.json"};
    bool includeValidationFindings{true};
    bool includeDependencyGraph{true};
};

struct ExecutionConfig {
    bool useTransactionDb{true};
    bool deleteCommittedTransactions{true};
    bool checkVirtualFileSystemWrite{true};
    bool stopOnFirstFailure{false};
    bool dryRun{false};
};

struct PlannerConfig {
    bool enableProxyExpansion{true};
    bool autoDownloadMissingPlugins{true};
    bool autoDownloadMissingDependencies{true};
    bool buildDependencyDag{true};
    bool topologicallySortGraph{true};
    std::map<std::string, std::string> systemAliases{};
};

struct DownloaderConfig {
    bool enabled{true};
    bool followRedirects{true};
    long connectTimeoutSeconds{10};
    long requestTimeoutSeconds{60};
    std::string userAgent{"ReqPack/0.1.0"};
    std::map<std::string, std::string> pluginSources{};
};

struct RegistrySourceEntry {
    std::string source{};
    bool alias{false};
    std::string description{};
};

using RegistrySourceMap = std::map<std::string, RegistrySourceEntry>;

struct RegistryConfig {
    std::string databasePath{"~/.reqpack/registry"};
    std::string remoteUrl{};
    std::string overlayPath{};
    RegistrySourceMap sources{};
    std::string pluginDirectory{"./plugins"};
    bool autoLoadPlugins{true};
    bool shutDownPluginsOnExit{true};
};

struct InteractionConfig {
    bool interactive{true};
    bool promptBeforeUnsafeActions{false};
    bool promptBeforeMissingPluginDownload{false};
    bool promptBeforeMissingDependencyDownload{false};
};

struct ReqPackConfig {
    std::string applicationName{"ReqPack"};
    std::string version{"0.1.0"};

    LoggingConfig logging{};
    SecurityConfig security{};
    ReportConfig reports{};
    ExecutionConfig execution{};
    PlannerConfig planner{};
    DownloaderConfig downloader{};
    RegistryConfig registry{};
    InteractionConfig interaction{};

    std::vector<std::string> enabledScanners{};
    std::vector<std::string> enabledReportFormats{};
};

struct ReqPackConfigOverrides {
    std::optional<std::filesystem::path> configPath;

    std::optional<LogLevel> logLevel;
    std::optional<std::string> logPattern;
    std::optional<bool> fileOutput;
    std::optional<std::string> logFilePath;
    std::optional<bool> enableBacktrace;
    std::optional<std::size_t> backtraceSize;

    std::optional<bool> runSnykScan;
    std::optional<bool> runOwaspScan;
    std::optional<SeverityLevel> severityThreshold;
    std::optional<double> scoreThreshold;
    std::optional<UnsafeAction> onUnsafe;
    std::optional<bool> promptOnUnsafe;

    std::optional<bool> reportEnabled;
    std::optional<ReportFormat> reportFormat;
    std::optional<std::string> reportOutputPath;

    std::optional<bool> dryRun;
    std::optional<bool> stopOnFirstFailure;
    std::optional<bool> useTransactionDb;

    std::optional<bool> enableProxyExpansion;

    std::optional<std::string> registryPath;
    std::optional<std::string> pluginDirectory;
    std::optional<bool> autoLoadPlugins;

    std::optional<bool> interactive;
};

inline const ReqPackConfig DEFAULT_REQPACK_CONFIG{};

std::optional<SeverityLevel> severity_level_from_string(const std::string& severity);
std::optional<LogLevel> log_level_from_string(const std::string& level);
std::optional<ReportFormat> report_format_from_string(const std::string& format);
std::optional<UnsafeAction> unsafe_action_from_string(const std::string& action);

std::filesystem::path reqpack_home_directory();
std::filesystem::path default_reqpack_config_path();
std::filesystem::path default_reqpack_registry_path();
std::filesystem::path registry_database_directory(const std::filesystem::path& registryPath);
std::filesystem::path registry_source_file_path(const std::filesystem::path& registryPath);

RegistrySourceMap load_registry_sources_from_lua(const std::filesystem::path& sourcePath);
RegistrySourceMap collect_registry_sources(const ReqPackConfig& config);

ReqPackConfig load_config_from_lua(
    const std::filesystem::path& configPath,
    const ReqPackConfig& fallback = DEFAULT_REQPACK_CONFIG
);

ReqPackConfig apply_config_overrides(const ReqPackConfig& base, const ReqPackConfigOverrides& overrides);

bool consume_cli_config_flag(
    const std::vector<std::string>& arguments,
    std::size_t& index,
    ReqPackConfigOverrides& overrides
);

ReqPackConfigOverrides extract_cli_config_overrides(int argc, char* argv[]);

inline std::string to_string(SeverityLevel severity) {
    switch (severity) {
        case SeverityLevel::CRITICAL:
            return "critical";
        case SeverityLevel::HIGH:
            return "high";
        case SeverityLevel::MEDIUM:
            return "medium";
        case SeverityLevel::LOW:
            return "low";
        case SeverityLevel::UNASSIGNED:
        default:
            return "unassigned";
    }
}

inline std::string to_string(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE:
            return "trace";
        case LogLevel::DEBUG:
            return "debug";
        case LogLevel::INFO:
            return "info";
        case LogLevel::WARN:
            return "warn";
        case LogLevel::ERROR:
            return "error";
        case LogLevel::CRITICAL:
        default:
            return "critical";
    }
}

inline std::string to_string(ReportFormat format) {
    switch (format) {
        case ReportFormat::JSON:
            return "json";
        case ReportFormat::CYCLONEDX:
            return "cyclonedx";
        case ReportFormat::NONE:
        default:
            return "none";
    }
}
