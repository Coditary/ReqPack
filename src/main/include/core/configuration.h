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

enum class OsvRefreshMode {
    MANUAL,
    PERIODIC,
    ALWAYS
};

enum class SbomOutputFormat {
    TABLE,
    JSON,
    CYCLONEDX_JSON
};

enum class DisplayRenderer {
    PLAIN,
    COLOR
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
    std::string osvDatabasePath{"~/.reqpack/osv"};
    std::string osvFeedUrl{"https://storage.googleapis.com/osv-vulnerabilities"};
    OsvRefreshMode osvRefreshMode{OsvRefreshMode::MANUAL};
    long osvRefreshIntervalSeconds{24L * 60L * 60L};
    std::string osvOverlayPath{};
    std::vector<std::string> ignoreVulnerabilityIds{};
    std::vector<std::string> allowVulnerabilityIds{};
    std::map<std::string, std::string> osvEcosystemMap{};
    UnsafeAction onUnresolvedVersion{UnsafeAction::CONTINUE};
    bool strictEcosystemMapping{false};
    bool includeWithdrawnInReport{false};
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
    std::string transactionDatabasePath{"~/.reqpack/transactions"};
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

struct RemoteConfig {
    bool readonly{false};
    int maxConnections{16};
};

struct SbomConfig {
    SbomOutputFormat defaultFormat{SbomOutputFormat::TABLE};
    std::string defaultOutputPath{};
    bool prettyPrint{true};
    bool includeDependencyEdges{true};
};

struct RqConfig {
    std::vector<std::string> repositories{};
    std::string statePath{"~/.reqpack/rq/state"};
};

struct HistoryConfig {
    // Controls history.jsonl – the append-only event log.
    bool enabled{true};

    // Controls installed-state tracking – current-state snapshot storage.
    // Independent of `enabled`: can be true even when `enabled` is false.
    bool trackInstalled{true};

    // Directory that holds history.jsonl and installed-state data.
    std::string historyPath{"~/.reqpack/history"};

    // Maximum number of lines kept in history.jsonl.
    // When exceeded, oldest entries are trimmed.  0 = unlimited.
    std::size_t maxLines{0};

    // Maximum file size of history.jsonl in megabytes.
    // When exceeded, oldest entries are trimmed.  0.0 = unlimited.
    double maxSizeMb{0.0};
};

struct DisplayColorScheme {
    std::string rule{};
    std::string header{"bold"};
    std::string summaryOk{"bold green"};
    std::string summaryFail{"bold red"};
    std::string barFill{"green"};
    std::string barEmpty{};
    std::string barOuter{};
    std::string step{"cyan"};
    std::string successMarker{"bold green"};
    std::string failureMarker{"bold red"};
    std::string message{};
};

struct DisplayConfig {
    DisplayRenderer renderer{DisplayRenderer::PLAIN};
    DisplayColorScheme colors{};
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
    RemoteConfig remote{};
    SbomConfig sbom{};
    RqConfig rq{};
    HistoryConfig history{};
    DisplayConfig display{};

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
    std::optional<std::string> osvDatabasePath;
    std::optional<std::string> osvFeedUrl;
    std::optional<OsvRefreshMode> osvRefreshMode;
    std::optional<long> osvRefreshIntervalSeconds;
    std::optional<std::string> osvOverlayPath;
    std::vector<std::string> ignoreVulnerabilityIds{};
    std::vector<std::string> allowVulnerabilityIds{};
    std::optional<UnsafeAction> onUnresolvedVersion;
    std::optional<bool> strictEcosystemMapping;
    std::optional<bool> includeWithdrawnInReport;

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

    std::optional<SbomOutputFormat> sbomDefaultFormat;
    std::optional<std::string> sbomDefaultOutputPath;
    std::optional<bool> sbomPrettyPrint;
    std::optional<bool> sbomIncludeDependencyEdges;
};

inline const ReqPackConfig DEFAULT_REQPACK_CONFIG{};

std::optional<SeverityLevel> severity_level_from_string(const std::string& severity);
std::optional<LogLevel> log_level_from_string(const std::string& level);
std::optional<ReportFormat> report_format_from_string(const std::string& format);
std::optional<UnsafeAction> unsafe_action_from_string(const std::string& action);
std::optional<OsvRefreshMode> osv_refresh_mode_from_string(const std::string& mode);
std::optional<SbomOutputFormat> sbom_output_format_from_string(const std::string& format);
std::optional<DisplayRenderer> display_renderer_from_string(const std::string& renderer);

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

ReqPackConfigOverrides extract_cli_config_overrides(const std::vector<std::string>& arguments);

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

inline std::string to_string(UnsafeAction action) {
    switch (action) {
        case UnsafeAction::PROMPT:
            return "prompt";
        case UnsafeAction::ABORT:
            return "abort";
        case UnsafeAction::CONTINUE:
        default:
            return "continue";
    }
}

inline std::string to_string(OsvRefreshMode mode) {
    switch (mode) {
        case OsvRefreshMode::PERIODIC:
            return "periodic";
        case OsvRefreshMode::ALWAYS:
            return "always";
        case OsvRefreshMode::MANUAL:
        default:
            return "manual";
    }
}

inline std::string to_string(SbomOutputFormat format) {
    switch (format) {
        case SbomOutputFormat::JSON:
            return "json";
        case SbomOutputFormat::CYCLONEDX_JSON:
            return "cyclonedx-json";
        case SbomOutputFormat::TABLE:
        default:
            return "table";
    }
}

inline std::string to_string(DisplayRenderer renderer) {
    switch (renderer) {
        case DisplayRenderer::COLOR:
            return "color";
        case DisplayRenderer::PLAIN:
        default:
            return "plain";
    }
}
