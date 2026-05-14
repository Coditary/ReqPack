#include "core/export/audit_exporter.h"

#include "audit_exporter_internal.h"

#include <algorithm>
#include <filesystem>
#include <string>

namespace {

bool request_has_flag(const Request& request, const std::string& name) {
    return std::find(request.flags.begin(), request.flags.end(), name) != request.flags.end();
}

}  // namespace

AuditExporter::AuditExporter(PluginMetadataProvider* metadataProvider, const ReqPackConfig& config)
    : config(config), metadataProvider(metadataProvider) {}

AuditOutputFormat AuditExporter::resolveFormat(const Request& request) const {
    if (!request.outputFormat.empty()) {
        const auto parsed = audit_output_format_from_string(request.outputFormat);
        if (parsed.has_value()) {
            return parsed.value();
        }
    }

    if (!request.outputPath.empty()) {
        const std::string extension =
            audit_exporter_internal::to_lower_copy(std::filesystem::path(request.outputPath).extension().string());
        if (extension == ".sarif") {
            return AuditOutputFormat::SARIF;
        }
        return AuditOutputFormat::CYCLONEDX_VEX_JSON;
    }

    return AuditOutputFormat::TABLE;
}

std::string AuditExporter::resolveOutputPath(const Request& request) const {
    return request.outputPath;
}

std::string AuditExporter::renderGraph(const Graph& graph, const std::vector<ValidationFinding>& findings, const Request& request) const {
    switch (resolveFormat(request)) {
        case AuditOutputFormat::JSON:
            return renderJson(graph, findings);
        case AuditOutputFormat::CYCLONEDX_VEX_JSON:
            return renderCycloneDxVex(graph, findings);
        case AuditOutputFormat::SARIF:
            return renderSarif(graph, findings);
        case AuditOutputFormat::TABLE:
        default:
            return renderTable(
                graph,
                findings,
                request.outputPath.empty() && audit_exporter_internal::table_colors_enabled(),
                request_has_flag(request, "no-wrap"),
                request_has_flag(request, "wide")
            );
    }
}
