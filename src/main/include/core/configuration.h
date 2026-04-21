#pragma once

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
};

struct RegistryConfig {
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
    RegistryConfig registry{};
    InteractionConfig interaction{};

    std::vector<std::string> enabledScanners{};
    std::vector<std::string> enabledReportFormats{};
};

inline const ReqPackConfig DEFAULT_REQPACK_CONFIG{};

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
