#include "core/export/sbom_exporter.h"

#include "sbom_exporter_internal.h"

#include <algorithm>
#include <string>

namespace {

bool request_has_flag(const Request& request, const std::string& name) {
    return std::find(request.flags.begin(), request.flags.end(), name) != request.flags.end();
}

}  // namespace

SbomExporter::SbomExporter(PluginMetadataProvider* metadataProvider, const ReqPackConfig& config)
    : config(config), metadataProvider(metadataProvider) {}

SbomOutputFormat SbomExporter::resolveFormat(const Request& request) const {
    if (!request.outputFormat.empty()) {
        const auto parsed = sbom_output_format_from_string(request.outputFormat);
        if (parsed.has_value()) {
            return parsed.value();
        }
    }

    if (!request.outputPath.empty()) {
        return SbomOutputFormat::CYCLONEDX_JSON;
    }

    return this->config.sbom.defaultFormat;
}

std::string SbomExporter::resolveOutputPath(const Request& request) const {
    if (!request.outputPath.empty()) {
        return request.outputPath;
    }

    return this->config.sbom.defaultOutputPath;
}

std::string SbomExporter::renderGraph(const Graph& graph, const Request& request) const {
    switch (resolveFormat(request)) {
        case SbomOutputFormat::JSON:
            return renderJson(graph, false);
        case SbomOutputFormat::CYCLONEDX_JSON:
            return renderJson(graph, true);
        case SbomOutputFormat::TABLE:
        default:
            return renderTable(
                graph,
                request.outputPath.empty() && sbom_exporter_internal::table_colors_enabled(),
                request_has_flag(request, "no-wrap"),
                request_has_flag(request, "wide")
            );
    }
}
