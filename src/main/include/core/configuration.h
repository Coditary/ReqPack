#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
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

enum class AuditOutputFormat {
    TABLE,
    JSON,
    CYCLONEDX_VEX_JSON,
    SARIF
};

enum class DisplayRenderer {
    PLAIN,
    COLOR
};

enum class RepositoryAuthType {
    NONE,
    BASIC,
    TOKEN,
    SSH
};

enum class RepositoryChecksumPolicy {
    FAIL,
    WARN,
    IGNORE
};

struct LoggingConfig {
    LogLevel level{LogLevel::INFO};
    bool consoleOutput{false};
    bool fileOutput{false};
    std::string filePath{"reqpack.log"};
    bool enableBacktrace{false};
    std::size_t backtraceSize{32};
    std::string pattern{"[%^%l%$] %v"};
};

struct SecurityGatewayConfig {
    bool enabled{true};
    std::vector<std::string> backends{};
};

struct SecurityBackendConfig {
    bool enabled{true};
    std::string feedUrl{};
    OsvRefreshMode refreshMode{OsvRefreshMode::MANUAL};
    long refreshIntervalSeconds{24L * 60L * 60L};
    std::string overlayPath{};
};

struct SecurityConfig {
    bool enabled{true};
    bool autoFetch{true};
    bool runSnykScan{false};
    bool runOwaspScan{false};
    SeverityLevel severityThreshold{SeverityLevel::CRITICAL};
    double scoreThreshold{0.0};
    UnsafeAction onUnsafe{UnsafeAction::CONTINUE};
    bool promptOnUnsafe{false};
    bool allowUnassigned{true};
    std::string defaultGateway{"security"};
    std::string cachePath{};
    std::string indexPath{};
    std::string osvDatabasePath{};
    std::string osvFeedUrl{"https://storage.googleapis.com/osv-vulnerabilities"};
    OsvRefreshMode osvRefreshMode{OsvRefreshMode::MANUAL};
    long osvRefreshIntervalSeconds{24L * 60L * 60L};
    std::string osvOverlayPath{};
    std::vector<std::string> ignoreVulnerabilityIds{};
    std::vector<std::string> allowVulnerabilityIds{};
    std::map<std::string, std::string> ecosystemMap{};
    std::map<std::string, std::string> osvEcosystemMap{};
    std::map<std::string, SecurityGatewayConfig> gateways{};
    std::map<std::string, SecurityBackendConfig> backends{};
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
    std::string transactionDatabasePath{};
};

struct ProxyConfig {
    std::string defaultTarget{};
    std::vector<std::string> targets{};
    std::map<std::string, std::string> options{};
};

struct PlannerConfig {
    bool enableProxyExpansion{true};
    bool autoDownloadMissingPlugins{true};
    bool autoDownloadMissingDependencies{true};
    bool buildDependencyDag{true};
    bool topologicallySortGraph{true};
    std::map<std::string, std::string> systemAliases{};
    std::map<std::string, ProxyConfig> proxies{};
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
    std::string databasePath{};
    std::string remoteUrl{};
    std::string overlayPath{};
    RegistrySourceMap sources{};
    std::string pluginDirectory{};
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
    bool skipMissingPackages{false};
};

struct RqpConfig {
    std::vector<std::string> repositories{};
    std::string statePath{};
};

struct SelfUpdateConfig {
    std::string repoUrl{"https://github.com/Coditary/ReqPack.git"};
    std::string branch{"main"};
    std::string repoPath{};
    std::string buildPath{};
    std::string binaryDirectory{};
    std::string linkPath{};
};

struct ArchiveConfig {
    std::string password{};
};

struct RepositoryAuthConfig {
    RepositoryAuthType type{RepositoryAuthType::NONE};
    std::string username{};
    std::string password{};
    std::string token{};
    std::string sshKey{};
    std::string headerName{};
};

struct RepositoryValidationConfig {
    RepositoryChecksumPolicy checksum{RepositoryChecksumPolicy::WARN};
    bool tlsVerify{true};
};

struct RepositoryScopeConfig {
    std::vector<std::string> include{};
    std::vector<std::string> exclude{};
};

using RepositoryExtraValue = std::variant<std::string, bool, double, std::vector<std::string>>;

struct RepositoryEntry {
    std::string id{};
    std::string url{};
    int priority{100};
    bool enabled{true};
    std::string type{};
    RepositoryAuthConfig auth{};
    RepositoryValidationConfig validation{};
    RepositoryScopeConfig scope{};
    std::map<std::string, RepositoryExtraValue> extras{};
};

struct HistoryConfig {
    // Controls history.jsonl – the append-only event log.
    bool enabled{true};

    // Controls installed-state tracking – current-state snapshot storage.
    // Independent of `enabled`: can be true even when `enabled` is false.
    bool trackInstalled{true};

    // Directory that holds history.jsonl and installed-state data.
    std::string historyPath{};

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
    RqpConfig rqp{};
    SelfUpdateConfig selfUpdate{};
    ArchiveConfig archives{};
    std::map<std::string, std::vector<RepositoryEntry>> repositories{};
    HistoryConfig history{};
    DisplayConfig display{};

    std::vector<std::string> enabledScanners{};
    std::vector<std::string> enabledReportFormats{};

    ReqPackConfig();
};

struct ReqPackConfigOverrides {
    std::optional<std::filesystem::path> configPath;

    std::optional<bool> consoleOutput;
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
    std::map<std::string, std::string> proxyDefaultTargets{};

    std::optional<std::string> registryPath;
    std::optional<std::string> pluginDirectory;
    std::optional<bool> autoLoadPlugins;

    std::optional<bool> interactive;
    std::optional<std::string> archivePassword;

    std::optional<SbomOutputFormat> sbomDefaultFormat;
    std::optional<std::string> sbomDefaultOutputPath;
    std::optional<bool> sbomPrettyPrint;
    std::optional<bool> sbomIncludeDependencyEdges;
    std::optional<bool> sbomSkipMissingPackages;
};

ReqPackConfig default_reqpack_config();

std::optional<SeverityLevel> severity_level_from_string(const std::string& severity);
std::optional<LogLevel> log_level_from_string(const std::string& level);
std::optional<ReportFormat> report_format_from_string(const std::string& format);
std::optional<UnsafeAction> unsafe_action_from_string(const std::string& action);
std::optional<OsvRefreshMode> osv_refresh_mode_from_string(const std::string& mode);
std::optional<SbomOutputFormat> sbom_output_format_from_string(const std::string& format);
std::optional<AuditOutputFormat> audit_output_format_from_string(const std::string& format);
std::optional<DisplayRenderer> display_renderer_from_string(const std::string& renderer);

std::filesystem::path reqpack_config_directory();
std::filesystem::path reqpack_data_directory();
std::filesystem::path reqpack_cache_directory();
std::filesystem::path default_reqpack_config_path();
std::filesystem::path default_reqpack_registry_path();
std::filesystem::path default_reqpack_plugin_directory();
std::filesystem::path default_reqpack_history_path();
std::filesystem::path default_reqpack_transaction_path();
std::filesystem::path default_reqpack_rqp_state_path();
std::filesystem::path default_reqpack_self_update_repo_path();
std::filesystem::path default_reqpack_self_update_build_path();
std::filesystem::path default_reqpack_self_update_binary_directory();
std::filesystem::path default_reqpack_self_update_link_path();
std::filesystem::path default_reqpack_security_cache_path();
std::filesystem::path default_reqpack_security_index_path();
std::filesystem::path default_reqpack_osv_database_path();
std::filesystem::path default_reqpack_repo_cache_path();
std::filesystem::path registry_database_directory(const std::filesystem::path& registryPath);
std::filesystem::path registry_source_file_path(const std::filesystem::path& registryPath);

RegistrySourceMap load_registry_sources_from_lua(const std::filesystem::path& sourcePath);
RegistrySourceMap collect_registry_sources(const ReqPackConfig& config);
std::vector<RepositoryEntry> repositories_for_ecosystem(const ReqPackConfig& config, const std::string& ecosystem);
std::optional<ProxyConfig> proxy_config_for_system(const ReqPackConfig& config, const std::string& system);

ReqPackConfig load_config_from_lua(
    const std::filesystem::path& configPath,
    const ReqPackConfig& fallback = default_reqpack_config()
);

ReqPackConfig apply_config_overrides(const ReqPackConfig& base, const ReqPackConfigOverrides& overrides);

std::string resolve_archive_password(const ReqPackConfig& config);

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

inline std::string to_string(AuditOutputFormat format) {
    switch (format) {
        case AuditOutputFormat::JSON:
            return "json";
        case AuditOutputFormat::CYCLONEDX_VEX_JSON:
            return "cyclonedx-vex-json";
        case AuditOutputFormat::SARIF:
            return "sarif";
        case AuditOutputFormat::TABLE:
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

inline std::string to_string(RepositoryAuthType type) {
    switch (type) {
        case RepositoryAuthType::BASIC:
            return "basic";
        case RepositoryAuthType::TOKEN:
            return "token";
        case RepositoryAuthType::SSH:
            return "ssh";
        case RepositoryAuthType::NONE:
        default:
            return "none";
    }
}

inline std::string to_string(RepositoryChecksumPolicy policy) {
    switch (policy) {
        case RepositoryChecksumPolicy::FAIL:
            return "fail";
        case RepositoryChecksumPolicy::IGNORE:
            return "ignore";
        case RepositoryChecksumPolicy::WARN:
        default:
            return "warn";
    }
}
