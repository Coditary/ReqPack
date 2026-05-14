#include "core/config/configuration.h"

#include "configuration_internal.h"

#include <thread>

std::optional<SeverityLevel> severity_level_from_string(const std::string& severity) {
    const std::string normalized = configuration_internal::to_lower_copy(severity);
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
    const std::string normalized = configuration_internal::to_lower_copy(level);
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
    const std::string normalized = configuration_internal::to_lower_copy(format);
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
    const std::string normalized = configuration_internal::to_lower_copy(action);
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
    const std::string normalized = configuration_internal::to_lower_copy(mode);
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
    const std::string normalized = configuration_internal::to_lower_copy(format);
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
    const std::string normalized = configuration_internal::to_lower_copy(format);
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
    const std::string normalized = configuration_internal::to_lower_copy(renderer);
    if (normalized == "plain") {
        return DisplayRenderer::PLAIN;
    }
    if (normalized == "color" || normalized == "colour") {
        return DisplayRenderer::COLOR;
    }

    return std::nullopt;
}

std::optional<ExecutionJobsMode> execution_jobs_mode_from_string(const std::string& mode) {
    const std::string normalized = configuration_internal::to_lower_copy(mode);
    if (normalized == "fixed") {
        return ExecutionJobsMode::FIXED;
    }
    if (normalized == "max") {
        return ExecutionJobsMode::MAX;
    }

    return std::nullopt;
}

std::size_t resolved_execution_jobs(const ReqPackConfig& config) {
    if (config.execution.jobsMode == ExecutionJobsMode::MAX) {
        const unsigned int concurrency = std::thread::hardware_concurrency();
        return concurrency == 0 ? 1u : static_cast<std::size_t>(concurrency);
    }

    return std::max<std::size_t>(1u, static_cast<std::size_t>(config.execution.jobs));
}
